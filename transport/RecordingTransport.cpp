#include "transport/RecordingTransport.h"
#include "codec/Opus.h"
#include "codec/Vp8.h"
#include "config/Config.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/DataReceiver.h"
#include "transport/RtpSenderState.h"
#include "transport/recp/RecControlHeader.h"
#include "transport/recp/RecStreamAddedEvent.h"
#include "transport/recp/RecStreamRemovedEvent.h"

namespace transport
{

// Placed last in queue during shutdown to reduce ref count when all jobs are complete.
// This means other jobs in the transport job queue do not have to have counters
class ShutdownJob : public jobmanager::CountedJob
{
public:
    explicit ShutdownJob(std::atomic_uint32_t& ownerCount) : CountedJob(ownerCount) {}

    void run() override {}
};

std::unique_ptr<RecordingTransport> createRecordingTransport(jobmanager::JobManager& jobManager,
    const config::Config& config,
    RecordingEndpoint* recordingEndpoint,
    const size_t endpointIdHash,
    const size_t streamIdHash,
    const SocketAddress& peer,
    const uint8_t aesKey[32],
    const uint8_t salt[12])
{
    return std::make_unique<RecordingTransport>(jobManager,
        config,
        recordingEndpoint,
        endpointIdHash,
        streamIdHash,
        peer,
        aesKey,
        salt);
}

RecordingTransport::RecordingTransport(jobmanager::JobManager& jobManager,
    const config::Config& config,
    RecordingEndpoint* recordingEndpoint,
    const size_t endpointIdHash,
    const size_t streamIdHash,
    const SocketAddress& remotePeer,
    const uint8_t aesKey[32],
    const uint8_t salt[12])
    : _isInitialized(false),
      _loggableId("RecordingTransport"),
      _config(config),
      _endpointIdHash(endpointIdHash),
      _streamIdHash(streamIdHash),
      _isRunning(true),
      _recordingEndpoint(recordingEndpoint),
      _peerPort(remotePeer),
      _jobCounter(0),
      _jobQueue(jobManager),
      _aes(nullptr),
      _ivGenerator(nullptr),
      _previousSequenceNumber(256), // TODO what is a reasonable number?
      _rolloverCounter(256), // TODO what is a reasonable number?
      _outboundSsrcCounters(256),
      _rtcp(config.recordingRtcp.reportInterval)
{
    logger::info("Recording client: %s", _loggableId.c_str(), _peerPort.toString().c_str());

    assert(_recordingEndpoint->isGood());
    assert(recordingEndpoint->getLocalPort().getFamily() == remotePeer.getFamily());

    _recordingEndpoint->registerRecordingListener(_peerPort, this);
    ++_jobCounter;

    _aes = std::make_unique<crypto::AES>(aesKey, 32);
    _ivGenerator = std::make_unique<crypto::AesGcmIvGenerator>(salt, 12);

    _isInitialized = true;

    if (recordingEndpoint->getLocalPort().getFamily() != remotePeer.getFamily())
    {
        logger::error("ip family mismatch. local ip family: %d, remote ip family: %d",
            _loggableId.c_str(),
            recordingEndpoint->getLocalPort().getFamily(),
            remotePeer.getFamily());
    }
}

bool RecordingTransport::start()
{
    return _isInitialized && _recordingEndpoint->isGood();
}

void RecordingTransport::stop()
{
    logger::debug("stopping jobcount %u, running %u", _loggableId.c_str(), _jobCounter.load(), _isRunning.load());
    if (!_isRunning)
    {
        return;
    }

    _recordingEndpoint->unregisterRecordingListener(this);

    _jobQueue.getJobManager().abortTimedJobs(getId());
    _isRunning = false;
}

void RecordingTransport::protectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator)
{
    if (!isConnected())
    {
        sendAllocator.free(packet);
        return;
    }

    protectAndSend(packet, _peerPort, _recordingEndpoint, sendAllocator);
}

void RecordingTransport::protectAndSend(memory::Packet* packet,
    const SocketAddress& target,
    Endpoint* endpoint,
    memory::PacketPoolAllocator& allocator)
{
    assert(packet->getLength() + 24 <= _config.mtu);

    if (rtp::isRtpPacket(*packet))
    {
        auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        auto roc = getRolloverCounter(rtpHeader->ssrc, rtpHeader->sequenceNumber);

        uint8_t iv[crypto::DEFAULT_AES_IV_SIZE];
        _ivGenerator->generateForRtp(rtpHeader->ssrc, roc, rtpHeader->sequenceNumber, iv, crypto::DEFAULT_AES_IV_SIZE);

        auto payload = rtpHeader->getPayload();

        auto headerLength = rtpHeader->headerLength();
        auto payloadLength = packet->getLength() - headerLength;
        uint16_t encryptedLength = _config.mtu - headerLength;

        _aes->gcmEncrypt(payload,
            payloadLength,
            reinterpret_cast<unsigned char*>(payload),
            encryptedLength,
            iv,
            crypto::DEFAULT_AES_IV_SIZE,
            reinterpret_cast<unsigned char*>(rtpHeader),
            headerLength);

        packet->setLength(headerLength + encryptedLength);

        const auto timestamp = utils::Time::getAbsoluteTime();

        auto* senderState = getOutboundSsrc(rtpHeader->ssrc);

        // Sometimes we can have a race and end up with a RTP packet before the add stream event or
        // we can receive a packet for a stream that was removed already.
        // Neither cases cause problems, we just need to force to send a RTCP in the first packet after the
        // stream has been registered (as we depend on it to calculate the mute time in beginning)
        // and be aware that senderState can be a nullptr
        const bool isFirstPacket = senderState && senderState->getLastSendTime() == 0;
        if (senderState)
        {
            senderState->onRtpSent(timestamp, *packet);
        }

        endpoint->sendTo(target, packet, allocator);

        if (isFirstPacket || _rtcp.lastSendTime == 0 ||
            utils::Time::diffGT(_rtcp.lastSendTime, timestamp, _rtcp.reportInterval))
        {
            sendRtcpSenderReport(allocator, timestamp);
        }
    }
    else if (recp::isRecPacket(reinterpret_cast<const unsigned char*>(packet->get()), packet->getLength()))
    {
        auto recHeader = recp::RecHeader::fromPacket(*packet);
        auto payload = recHeader->getPayload();

        if (recHeader->event == recp::RecEventType::StreamAdded)
        {
            onSendingStreamAddedEvent(packet);
        }
        else if (recHeader->event == recp::RecEventType::StreamRemoved)
        {
            onSendingStreamRemovedEvent(packet);
        }

        uint8_t iv[crypto::DEFAULT_AES_IV_SIZE];
        _ivGenerator->generateForRec(static_cast<uint8_t>(recHeader->event),
            recHeader->sequenceNumber,
            recHeader->timestamp,
            iv,
            crypto::DEFAULT_AES_IV_SIZE);

        auto payloadLength = packet->getLength() - recp::REC_HEADER_SIZE;
        uint16_t encryptedLength = _config.mtu - recp::REC_HEADER_SIZE;

        _aes->gcmEncrypt(payload,
            payloadLength,
            reinterpret_cast<unsigned char*>(payload),
            encryptedLength,
            iv,
            crypto::DEFAULT_AES_IV_SIZE,
            reinterpret_cast<unsigned char*>(recHeader),
            recp::REC_HEADER_SIZE);

        packet->setLength(recp::REC_HEADER_SIZE + encryptedLength);
        endpoint->sendTo(target, packet, allocator);
    }
    else if (rtp::isRtcpPacket(*packet))
    {
        endpoint->sendTo(target, packet, allocator);
    }
    else
    {
        allocator.free(packet);
        logger::debug("DIDN'T send packet to %s", getLoggableId().c_str(), target.toString().c_str());
    }
}

bool RecordingTransport::unprotect(memory::Packet* packet)
{
    // TODO implement payload decryption
    return false;
}

bool RecordingTransport::isConnected()
{
    return _isRunning && _recordingEndpoint->isGood();
}

void RecordingTransport::setDataReceiver(DataReceiver* dataReceiver)
{
    _dataReceiver.store(dataReceiver);
}

uint32_t RecordingTransport::getRolloverCounter(uint32_t ssrc, uint16_t sequenceNumber)
{
    auto previousSequenceNumber = _previousSequenceNumber.find(ssrc);
    if (previousSequenceNumber != _previousSequenceNumber.cend())
    {
        auto roc = _rolloverCounter.find(ssrc);
        assert(roc != _rolloverCounter.cend());

        int32_t diff = previousSequenceNumber->second - sequenceNumber;
        auto quarter = 1 << 14;
        // if sequence number is more than a quarter-turn away it's a rollover
        if (diff > quarter)
        {
            previousSequenceNumber->second = sequenceNumber;
            return ++roc->second;
        }
        // if sequence number is less than a quarter-turn away negatively it's a reordering
        else if (diff < -quarter)
        {
            return roc->second - 1;
        }
        else if (sequenceNumber > previousSequenceNumber->second)
        {
            previousSequenceNumber->second = sequenceNumber;
            return roc->second;
        }
        else
        {
            return roc->second;
        }
    }
    else
    {
        _previousSequenceNumber.emplace(ssrc, sequenceNumber);
        _rolloverCounter.emplace(ssrc, 0);
        return 0;
    }
}

void RecordingTransport::sendRtcpSenderReport(memory::PacketPoolAllocator& sendAllocator, uint64_t timestamp)
{
    auto* rtcpPacket = memory::makePacket(sendAllocator);
    if (!rtcpPacket)
    {
        logger::warn("Not enough memory to send SR RTCP", _loggableId.c_str());
        return;
    }

    constexpr int MINIMUM_SR = 7 * sizeof(uint32_t);

    const auto wallClock = std::chrono::system_clock::now();

    for (auto& it : _outboundSsrcCounters)
    {
        const size_t remaining = _config.recordingRtcp.mtu - rtcpPacket->getLength();
        if (remaining < MINIMUM_SR + sizeof(rtp::ReportBlock))
        {
            protectAndSend(rtcpPacket, sendAllocator);
            _rtcp.lastSendTime = timestamp;
            rtcpPacket = memory::makePacket(sendAllocator);
            if (!rtcpPacket)
            {
                logger::warn("Not enough memory to send SR RTCP", _loggableId.c_str());
                return;
            }
        }

        auto* senderReport = rtp::RtcpSenderReport::create(rtcpPacket->get() + rtcpPacket->getLength());
        senderReport->ssrc = it.first;
        it.second.fillInReport(*senderReport, timestamp, utils::Time::toNtp(wallClock));
        rtcpPacket->setLength(senderReport->header.size() + rtcpPacket->getLength());
        assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket));
    }

    if (!rtcpPacket)
    {
        return;
    }

    if (rtcpPacket->getLength() > 0)
    {
        protectAndSend(rtcpPacket, sendAllocator);
        _rtcp.lastSendTime = timestamp;
    }
    else
    {
        sendAllocator.free(rtcpPacket);
    }
}

void RecordingTransport::onSendingStreamAddedEvent(memory::Packet* packet)
{
    const auto* recordingEvent = recp::RecStreamAddedEvent::fromPacket(*packet);
    assert(packet->getLength() >= recp::RecStreamAddedEvent::MIN_SIZE);
    assert(recordingEvent->header.event == recp::RecEventType::StreamAdded);

    uint32_t frequency = 0;

    switch (recordingEvent->rtpPayload)
    {
    case codec::Opus::payloadType:
        frequency = codec::Opus::sampleRate;
        break;
    case codec::Vp8::payloadType:
        frequency = codec::Vp8::sampleRate;
        break;
    default:
        logger::error("Payload type %d not recognized", _loggableId.c_str(), recordingEvent->rtpPayload);
        break;
    }

    if (frequency != 0)
    {
        _outboundSsrcCounters.emplace(recordingEvent->ssrc, frequency, _config);
    }
}

void RecordingTransport::onSendingStreamRemovedEvent(memory::Packet* packet)
{
    const auto* recordingEvent = recp::RecStreamRemovedEvent::fromPacket(*packet);
    assert(packet->getLength() >= recp::RecStreamRemovedEvent::MIN_SIZE);
    assert(recordingEvent->header.event == recp::RecEventType::StreamRemoved);

    _outboundSsrcCounters.erase(recordingEvent->ssrc);
}

RtpSenderState* RecordingTransport::getOutboundSsrc(const uint32_t ssrc)
{
    auto ssrcIt = _outboundSsrcCounters.find(ssrc);
    if (ssrcIt != _outboundSsrcCounters.cend())
    {
        return &ssrcIt->second;
    }

    return nullptr;
}

void RecordingTransport::onUnregistered(RecordingEndpoint& endpoint)
{
    logger::debug("Unregistered %s, %p", _loggableId.c_str(), endpoint.getName(), this);
    logger::debug("Recording transport events stopped jobcount %u", _loggableId.c_str(), _jobCounter.load() - 1);
    _jobQueue.addJob<ShutdownJob>(_jobCounter);
    _jobQueue.getJobManager().abortTimedJobs(getId());
    _isInitialized = false;
    --_jobCounter;
}

void RecordingTransport::onRecControlReceived(RecordingEndpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator)
{
    DataReceiver* const dataReceiver = _dataReceiver.load();
    if (!dataReceiver)
    {
        allocator.free(packet);
        return;
    }

    if (!recp::isRecControlPacket(packet->get(), packet->getLength()))
    {
        allocator.free(packet);
        return;
    }

    const auto receiveTime = utils::Time::getAbsoluteTime();
    dataReceiver->onRecControlReceived(this, packet, allocator, receiveTime);
}
} // namespace transport
