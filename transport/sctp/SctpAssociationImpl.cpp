#include "SctpAssociationImpl.h"
#include "SctpConfig.h"
#include "SctpServerPort.h"
#include "Sctprotocol.h"
#include "logger/Logger.h"
#include "memory/Array.h"
#include "utils/MersienneRandom.h"
#include "utils/Time.h"

#define SCTP_LOG_ENABLE 0

#if SCTP_LOG_ENABLE
#define SCTP_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
// silence compiler warnings
#define SCTP_LOG(fmt, logId, ...) (void)logId;
#endif

namespace sctp
{
const char* toString(sctp::SctpAssociation::State state)
{
    using namespace sctp;
    switch (state)
    {
    case SctpAssociation::State::CLOSED:
        return "CLOSED";
    case SctpAssociation::State::COOKIE_ECHOED:
        return "COOKIE_ECHOED";
    case SctpAssociation::State::COOKIE_WAIT:
        return "COOKIE_WAIT";
    case SctpAssociation::State::ESTABLISHED:
        return "ESTABLISHED";
    case SctpAssociation::State::SHUTDOWN_ACK_SENT:
        return "SHUTDOWN_ACK_SENT";
    case SctpAssociation::State::SHUTDOWN_PENDING:
        return "SHUTDOWN_PENDING";
    case SctpAssociation::State::SHUTDOWN_RECEIVED:
        return "SHUTDOWN_RECEIVED";
    case SctpAssociation::State::SHUTDOWN_SENT:
        return "SHUTDOWN_SENT";
    }
    return "unknown";
}

int64_t diff64(uint64_t a, uint64_t b)
{
    return static_cast<int64_t>(b - a);
}

SctpAssociationImpl::RTT::RTT(const SctpConfig& config)
    : _config(config),
      _peak(0.2 * timer::sec),
      _smoothed(0),
      _meanDeviation(0)
{
}

// use mean deviation instead of variance since it is cheaper to compute and we prefer larger offset from mean RTT.
void SctpAssociationImpl::RTT::update(uint64_t rttns)
{
    rttns = std::min(rttns, _config.RTO.max * utils::Time::ms);
    if (_smoothed == 0 && _meanDeviation == 0)
    {
        // first RTT received
        _smoothed = static_cast<double>(std::max(rttns, _config.RTO.min * utils::Time::ms));
        _meanDeviation = _smoothed / 2;
    }
    else
    {
        const auto rtt = static_cast<double>(rttns);
        const auto rttError = rtt - _smoothed;
        _meanDeviation += _config.RTO.beta * (std::abs(rttError) - _meanDeviation);
        _smoothed += _config.RTO.alpha * rttError;
        assert(_smoothed >= 0);
    }
    _peak = _smoothed + 4 * _meanDeviation;
}

double SctpAssociationImpl::RTT::getPeak() const
{
    return std::max(static_cast<double>(_config.RTO.min * timer::ms),
        std::min(_peak, static_cast<double>(_config.RTO.max * timer::ms)));
}

const size_t SctpAssociationImpl::MTU::MTU_SIZES[] = {128,
    256,
    512,
    640,
    768,
    960,
    1024,
    1152,
    1280,
    1408,
    1464, // likely when encapsulated in DTLS
    1472,
    1480,
    1536,
    1792,
    2048,
    4096,
    5120,
    6144,
    7168,
    8192,
    16384,
    32768};

void SctpAssociationImpl::MTU::pickInitialProbe()
{
    transmitCount = 0;

    for (auto probeIter = std::begin(MTU_SIZES); probeIter != std::end(MTU_SIZES) && *probeIter < maxMTU; ++probeIter)
    {
        if (*probeIter >= current)
        {
            selectedProbe = probeIter;
            return;
        }
    }
    selectedProbe = std::begin(MTU_SIZES);
}

SctpAssociationImpl::SentDataChunk::SentDataChunk(uint16_t streamId_,
    uint32_t payloadProtocol_,
    uint16_t streamSequenceNumber_,
    bool fragmentBegin_,
    bool fragmentEnd_,
    uint32_t transmissionSequenceNumber,
    const void* payload,
    size_t size_)
    : transmitTime(0),
      size(size_),
      transmissionSequenceNumber(transmissionSequenceNumber),
      streamId(streamId_),
      streamSequenceNumber(streamSequenceNumber_),
      payloadProtocol(payloadProtocol_),
      transmitCount(0),
      nackCount(0),
      fragmentBegin(fragmentBegin_),
      fragmentEnd(fragmentEnd_),
      reserved0(0),
      reserved1(0)
{
    std::memcpy(data(), payload, size_);
}

SctpAssociationImpl::ReceivedDataChunk::ReceivedDataChunk(const PayloadDataChunk& chunk, uint64_t timestamp)
    : receiveTime(timestamp),
      size(chunk.payloadSize()),
      transmissionSequenceNumber(chunk.transmissionSequenceNumber),
      streamId(chunk.streamId),
      streamSequenceNumber(chunk.streamSequenceNumber),
      payloadProtocol(chunk.payloadProtocol),
      receiveCount(1),
      fragmentBegin(chunk.isBegin()),
      fragmentEnd(chunk.isEnd())
{
    std::memcpy(data(), chunk.data(), chunk.payloadSize());
}

SctpAssociationImpl::TransmissionControlBlock::TransmissionControlBlock(uint16_t port_,
    uint32_t tag_,
    uint32_t receiveWindow,
    uint16_t inboundStreams_,
    uint16_t outboundStreams_)
    : port(port_),
      tag(tag_),
      tieTag(0),
      tsn(tag_),
      advertisedReceiveWindow(receiveWindow),
      inboundStreamCount(inboundStreams_),
      outboundStreamCount(outboundStreams_),
      cumulativeAck(0)
{
}

size_t SctpAssociationImpl::GenericCookie::maxSize() const
{
    return sizeof(cookie);
}

void SctpAssociationImpl::GenericCookie::set(const ChunkParameter& param)
{
    std::memcpy(cookie, param.data(), std::min(static_cast<size_t>(param.dataSize()), sizeof(cookie)));
    length = param.dataSize();
}

SctpAssociationImpl::ConnectionEstablishment::ConnectionEstablishment(const SctpConfig& config)
    : retransmitCount(0),
      initTimer(config.RTO.initial, config.RTO.max),
      cookieTimer(config.RTO.initial, config.RTO.max),
      timerBackOff(1.0)
{
}

SctpAssociationImpl::CongestionControl::CongestionControl(const SctpConfig& config, const logger::LoggableId& logId)
    : congestionWindow(4 * 1500),
      slowStartThreshold(config.flow.slowStartThreshold),
      idleTimer(config.RTO.initial, config.RTO.max),
      retransmitTimer(config.RTO.initial, config.RTO.max),
      inFastRecovery(false),
      fastRecoveryExitPoint(0),
      retransmitTimeout0(config.RTO.initial * timer::ms),
      _retransmitBackOffFactor(1.0),
      _partialBytesAcked(0),
      _loggableId(logId),
      _config(config)
{
}

void SctpAssociationImpl::CongestionControl::reset(uint32_t mtu, size_t slowStartThreshold_)
{
    congestionWindow = std::min(4 * mtu, std::max(2 * mtu, 4380u));
    slowStartThreshold = slowStartThreshold_;
}

// called on RTT interval to reduce congestion window when nothing is sent
void SctpAssociationImpl::CongestionControl::onIdle(uint64_t timestamp, uint32_t mtu)
{
    if (inFastRecovery)
    {
        return;
    }

    congestionWindow = std::max(congestionWindow / 2, 4 * mtu);
}

// call only if sack advanced cumulative ack
void SctpAssociationImpl::CongestionControl::onSackReceived(uint64_t timestamp,
    uint32_t mtu,
    uint32_t flightSizeBeforeAck,
    uint32_t bytesAcked,
    bool allPendingDataAcked)
{
    if (inFastRecovery)
    {
        return;
    }

    if (flightSizeBeforeAck >= congestionWindow)
    {
        if (congestionWindow < slowStartThreshold)
        {
            // "slow" start
            congestionWindow += std::min(bytesAcked, mtu);
            _partialBytesAcked = 0;
        }
        else
        {
            // congestion control
            // this practically grows window per RTT but only if we transmitted the entire window
            _partialBytesAcked += bytesAcked;
            if (_partialBytesAcked >= congestionWindow)
            {
                congestionWindow += mtu;
                _partialBytesAcked -= congestionWindow;
            }
        }
    }

    if (allPendingDataAcked)
    {
        _partialBytesAcked = 0;
    }
}

void SctpAssociationImpl::CongestionControl::onPacketLoss(uint64_t timestamp, uint32_t mtu)
{
    SCTP_LOG("cutting down congestion window due to loss", _loggableId.c_str());
    _partialBytesAcked = 0;
    slowStartThreshold = std::max(congestionWindow / 2, 4 * mtu);
    congestionWindow = slowStartThreshold;
}

void SctpAssociationImpl::CongestionControl::onTransmitTimeout(uint64_t timestamp, uint32_t mtu)
{
    SCTP_LOG("cutting down congestion window due to timeout", _loggableId.c_str());
    _partialBytesAcked = 0;
    slowStartThreshold = std::max(congestionWindow / 2, 4 * mtu);
    congestionWindow = mtu;
}

uint64_t SctpAssociationImpl::CongestionControl::getRetransmitTimeout() const
{
    return std::min(static_cast<uint64_t>(retransmitTimeout0 * _retransmitBackOffFactor), _config.RTO.max * timer::ms);
}

void SctpAssociationImpl::CongestionControl::doubleRetransmitTimeout()
{
    _retransmitBackOffFactor = std::min(_retransmitBackOffFactor * 2.0, 128.0);
}

void SctpAssociationImpl::CongestionControl::resetRetransmitTimeout()
{
    _retransmitBackOffFactor = 1.0;
}

SctpAssociationImpl::SctpAssociationImpl(size_t logId,
    SctpServerPort& transport,
    uint16_t remotePort,
    IEvents* listener,
    const SctpConfig& config)
    : _loggableId("SctpAssociation", logId),
      _config(config),
      _transport(transport),
      _listener(listener),
      _state(State::CLOSED),
      _local(transport.getPort(), _randomGenerator.next(), _config.receiveWindow.initial, 1, 1),
      _peer(remotePort, 0, 0, 0, 0),
      _connect(config),
      _rtt(config),
      _mtu(config.mtu.initial, config.mtu.max),
      _outboundBuffer(config.transmitBufferSize),
      _inboundBuffer(config.receiveBufferSize),
      _flow(config, _loggableId),
      _streamIdCounter(0)
{
    _flow.reset(_mtu.current, _config.flow.slowStartThreshold);
    if (_local.tag == 0)
    {
        ++_local.tag;
    }
    _peer.cumulativeAck = _local.tsn - 1;
}

SctpAssociationImpl::SctpAssociationImpl(size_t logId,
    SctpServerPort& transport,
    const SctpPacket& cookieEcho,
    IEvents* listener,
    const SctpConfig& config)
    : _loggableId("SctpAssociation", logId),
      _config(config),
      _transport(transport),
      _listener(listener),
      _state(State::CLOSED),
      _local(cookieEcho.getHeader().destinationPort,
          cookieEcho.getHeader().verificationTag,
          _config.receiveWindow.initial,
          0,
          0),
      _peer(0, 0, 0, 0, 0),
      _connect(config),
      _rtt(config),
      _mtu(config.mtu.initial, config.mtu.max),
      _outboundBuffer(config.transmitBufferSize),
      _inboundBuffer(config.receiveBufferSize),
      _flow(config, _loggableId),
      _streamIdCounter(1)
{
    const auto& header = cookieEcho.getHeader();

    _peer.port = header.sourcePort;
    _flow.reset(_mtu.current, _config.flow.slowStartThreshold);
    _peer.cumulativeAck = _local.tsn - 1;

    auto cookieChunk = cookieEcho.getChunk<CookieEchoChunk>(ChunkType::COOKIE_ECHO);
    if (cookieChunk)
    {
        auto& cookie = cookieChunk->getCookie<SctpCookie>();
        _peer.tag = cookie.tag.peer;
        _peer.tsn = cookie.peerTSN;
        _local.cumulativeAck = _peer.tsn - 1;
        _peer.advertisedReceiveWindow = cookie.peerReceiveWindow;
        _local.inboundStreamCount = cookie.inboundStreams;
        _local.outboundStreamCount = cookie.outboundStreams;
    }
}

// must only be called once and immediately after creation due to COOKIE ECHO received
void SctpAssociationImpl::onCookieEcho(const SctpPacket& cookieEcho, const uint64_t timestamp)
{
    SctpPacketW outboundPacket(_peer.tag, _local.port, _peer.port);
    outboundPacket.addChunk<GenericChunk>(ChunkType::COOKIE_ACK);
    setState(State::ESTABLISHED);
    _transport.send(outboundPacket);
}

bool SctpAssociationImpl::sendMessage(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* payloadData,
    size_t length,
    uint64_t timestamp)
{
    auto streamIt = _streams.find(streamId);
    if (_state < State::ESTABLISHED || streamIt == _streams.cend())
    {
        logger::warn("SCTP stream not open yet %u, count %zu", _loggableId.c_str(), streamId, _streams.size());
        return false;
    }
    if (length == 0)
    {
        return true;
    }

    const size_t payloadMtu = _mtu.current - PAYLOAD_DATA_OVERHEAD;
    const size_t pktCount = (length > payloadMtu ? (length + payloadMtu - 1) / payloadMtu : 1);
    const size_t payloadSize = std::min(1 + length / pktCount, payloadMtu);
    if (pktCount * sizeof(SentDataChunk) + length > _outboundBuffer.capacity())
    {
        return false;
    }
    auto& streamState = streamIt->second;
    auto* payloadBytes = reinterpret_cast<const uint8_t*>(payloadData);
    size_t writtenBytes = 0;
    for (size_t i = 0; i < pktCount; ++i)
    {
        const auto toWrite = std::min(length, payloadSize);
        auto* chunk = _outboundBuffer.instantiate<SentDataChunk>(toWrite,
            streamId,
            payloadProtocol,
            streamState.sequenceCounter,
            i == 0,
            i == (pktCount - 1),
            _local.tsn,
            payloadBytes + writtenBytes,
            toWrite);

        assert(chunk);
        if (!chunk)
        {
            logger::error("SCTP chunk buffer depleted %zuB left. %zu chunks pending. Dropped %zuB",
                _loggableId.c_str(),
                _outboundBuffer.capacity(),
                _outboundDataChunks.size(),
                length + writtenBytes);
            for (size_t k = 0; k < i; ++k)
            {
                auto addedChunk = _outboundDataChunks.back();
                _outboundDataChunks.pop_back();
                _outboundBuffer.free(addedChunk);
            }
            return false;
        }

        length -= toWrite;
        writtenBytes += toWrite;
        _outboundDataChunks.push_back(chunk);
        ++_local.tsn;
    }
    ++streamState.sequenceCounter;
    processOutboundChunks(timestamp);
    return true;
}

void SctpAssociationImpl::startMtuProbing(const uint64_t timestamp)
{
    if (_mtu.probing)
    {
        return;
    }
    _mtu.probing = true;
    _mtu.pickInitialProbe();
    sendMtuProbe(timestamp);
}

void SctpAssociationImpl::sendMtuProbe(const uint64_t timestamp)
{
    uint8_t packetArea[*_mtu.selectedProbe + 16];
    SctpPacketW probe(_local.tag, _local.port, _peer.port, packetArea, sizeof(packetArea));
    auto& heartbeat = probe.appendChunk<GenericChunk>(HEARTBEAT);

    auto& info = appendParameter<HeartbeatInfoParameter>(heartbeat);
    info.timestamp = timestamp;
    info.mtu = *_mtu.selectedProbe;
    info.sequenceNumber = 0;
    info.nonce = _randomGenerator.next();
    heartbeat.commitAppendedParameter();

    auto padding = *_mtu.selectedProbe - probe.size() - heartbeat.size();
    auto& padParam = appendParameter<PaddingParameter>(heartbeat);
    padParam.clearPadding(padding);
    heartbeat.commitAppendedParameter();
    probe.commitAppendedChunk();

    _mtu.probeTimer.startNs(timestamp, _flow.retransmitTimeout0);
    ++_mtu.transmitCount;
    _transport.send(probe);
}

void SctpAssociationImpl::setAdvertisedReceiveWindow(uint32_t size)
{
    _local.advertisedReceiveWindow = std::min(size, static_cast<uint32_t>(_inboundBuffer.capacity()));
}

void SctpAssociationImpl::setState(State newState)
{
    if (newState != _state)
    {
        _state = newState;
        _listener->onSctpStateChanged(this, newState);
        if (_state == State::ESTABLISHED)
        {
            _listener->onSctpEstablished(this);
        }
    }
}

int64_t SctpAssociationImpl::nextTimeout(const uint64_t timestamp)
{
    if (_state == State::CLOSED)
    {
        return -1;
    }
    int64_t minTimeout = 30 * timer::sec;

    minTimeout = std::min(minTimeout, _connect.cookieTimer.remainingTime(timestamp));
    minTimeout = std::min(minTimeout, _connect.initTimer.remainingTime(timestamp));
    minTimeout = std::min(minTimeout, _flow.retransmitTimer.remainingTime(timestamp));
    minTimeout = std::min(minTimeout, _mtu.probeTimer.remainingTime(timestamp));
    minTimeout = std::min(minTimeout, _flow.idleTimer.remainingTime(timestamp));
    return minTimeout;
}

int64_t SctpAssociationImpl::processTimeout(const uint64_t timestamp)
{
    const auto toSleep = nextTimeout(timestamp);
    if (toSleep != 0)
    {
        return toSleep;
    }

    if (_connect.cookieTimer.hasExpired(timestamp))
    {
        if (_state == State::COOKIE_ECHOED)
        {
            if (_connect.retransmitCount < _config.init.maxRetransmits)
            {
                ++_connect.retransmitCount;
                _connect.timerBackOff *= 2.0;
                _connect.cookieTimer.startMs(timestamp, _connect.timerBackOff * _config.RTO.initial);
                retransmitCookie();
            }
            else
            {
                _connect.cookieTimer.stop();
                setState(State::CLOSED);
            }
        }
        else
        {
            _connect.cookieTimer.stop();
        }
    }
    if (_connect.initTimer.hasExpired(timestamp))
    {
        if (_connect.retransmitCount < _config.init.maxRetransmits && _state == State::COOKIE_WAIT)
        {
            ++_connect.retransmitCount;
            _connect.timerBackOff *= 2.0;
            _connect.initTimer.startMs(timestamp, _connect.timerBackOff * _config.RTO.initial);
            sendInit();
        }
        else
        {
            _connect.initTimer.stop();
            setState(State::CLOSED);
        }
    }
    if (_state == State::ESTABLISHED)
    {
        if (_flow.retransmitTimer.hasExpired(timestamp))
        {
            logger::debug("retransmit timer expired %" PRIu64 "ms",
                _loggableId.c_str(),
                _flow.getRetransmitTimeout() / timer::ms);
            processOutboundChunks(timestamp);
        }

        if (_mtu.probeTimer.hasExpired(timestamp))
        {
            if (_mtu.transmitCount >= _config.flow.maxRetransmits)
            {
                _mtu.probing = false;
                _mtu.probeTimer.stop();
            }
            else
            {
                ++_mtu.transmitCount;
                sendMtuProbe(timestamp);
            }
        }

        if (_flow.idleTimer.hasExpired(timestamp))
        {
            if (_outboundDataChunks.empty())
            {
                _flow.onIdle(timestamp, _mtu.current);
                _flow.idleTimer.startNs(timestamp, _rtt.getPeak());
            }
            else
            {
                _flow.idleTimer.stop();
            }
        }
    }

    return nextTimeout(timestamp);
}

void SctpAssociationImpl::sendInit()
{
    SctpPacketW sctpPacket(0, _local.port, _peer.port);

    auto& initChunk = sctpPacket.appendChunk<InitChunk>();
    initChunk.inboundStreams = _local.inboundStreamCount;
    initChunk.outboundStreams = _local.outboundStreamCount;
    initChunk.initTag = _local.tag;
    initChunk.initTSN = _local.tsn;
    initChunk.advertisedReceiverWindow = _config.receiveWindow.initial;

    sctpPacket.commitAppendedChunk();
    _transport.send(sctpPacket);
    logger::info("SCTP sending INIT", _loggableId.c_str());
}

void SctpAssociationImpl::retransmitCookie()
{
    SctpPacketW outboundPacket(_peer.tag, _local.port, _peer.port);
    auto& echoChunk = outboundPacket.appendChunk<CookieEchoChunk>();
    echoChunk.setCookie(_connect.echoedCookie.cookie, _connect.echoedCookie.length);
    outboundPacket.commitAppendedChunk();
    _transport.send(outboundPacket);
}

void SctpAssociationImpl::connect(uint16_t inboundStreamCount, uint16_t outboundStreamCount, const uint64_t timestamp)
{
    _local.inboundStreamCount = inboundStreamCount;
    _local.outboundStreamCount = outboundStreamCount;
    _streamIdCounter = 0; // we seem to be client side

    sendInit();
    _connect.initTimer.startMs(timestamp, _config.init.timeout);
    setState(State::COOKIE_WAIT);
}

uint16_t SctpAssociationImpl::allocateStream()
{
    while (_streams.find(_streamIdCounter) != _streams.cend())
        _streamIdCounter += 2;

    _streams.emplace(std::forward_as_tuple(_streamIdCounter, _streamIdCounter));
    return _streamIdCounter;
}

// returns time to next timeout event
int64_t SctpAssociationImpl::onPacketReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    const auto& requestHeader = sctpPacket.getHeader();
    if (_local.port != requestHeader.destinationPort || _peer.port != requestHeader.sourcePort)
    {
        _listener->onSctpChunkDropped(this, sctpPacket.size());
        return nextTimeout(timestamp);
    }

    for (const ChunkField& chunk : sctpPacket.chunks())
    {
        switch (chunk.header.type)
        {
        case ChunkType::INIT_ACK:
            onInitAck(sctpPacket, reinterpret_cast<const InitAckChunk&>(chunk), timestamp);
            break;
        case ChunkType::COOKIE_ACK:
            onCookieAckReceived(sctpPacket, timestamp);
            break;
        case ChunkType::ABORT:
            onAbortReceived(sctpPacket, timestamp);
            break;
        case ChunkType::ERROR:
            onErrorReceived(sctpPacket, timestamp);
            break;
        case ChunkType::SHUTDOWN:
            onShutDownReceived(sctpPacket, timestamp);
            break;
        case ChunkType::SHUTDOWN_ACK:
            onShutDownAckReceived(sctpPacket, timestamp);
            break;
        case ChunkType::SHUTDOWN_COMPLETE:
            onShutDownCompleteReceived(sctpPacket, timestamp);
            break;
        case ChunkType::DATA:
        case ChunkType::SACK:
            onDataReceived(sctpPacket, timestamp);
            return nextTimeout(timestamp);
        case ChunkType::COOKIE_ECHO:
            onUnexpectedCookieEcho(sctpPacket, timestamp);
            break;
        case ChunkType::INIT:
            onUnexpectedInitReceived(sctpPacket, timestamp);
            break;
        case ChunkType::HEARTBEAT:
            onHeartbeatRequest(sctpPacket, timestamp);
            break;
        case ChunkType::HEARTBEAT_ACK:
            onHeartbeatResponse(sctpPacket, timestamp);
            break;
        case ChunkType::FORWARDTSN:
            logger::warn("Forward TSN chunk received. Not implemented", _loggableId.c_str());
            break;
        case ChunkType::RE_CONFIG:
            logger::warn("Reconfig chunk received. Peer is trying to reset streams.", _loggableId.c_str());
            break;
        default:
            logger::warn("unrecognized chunk %u", _loggableId.c_str(), chunk.header.type);
            _listener->onSctpChunkDropped(this, chunk.size());
        }
    }
    return nextTimeout(timestamp);
}

void SctpAssociationImpl::sendErrorResponse(const SctpPacket& request, const CauseCode& errorCause)
{
    SctpPacketW response(_peer.tag, _local.port, _peer.port);
    auto& abortChunk = response.appendChunk<AbortChunk>(true);

    abortChunk.add(errorCause);
    response.commitAppendedChunk();

    _transport.send(response);
}

std::tuple<uint16_t, uint16_t> SctpAssociationImpl::getPortPair() const
{
    return std::tuple<uint16_t, uint16_t>(_local.port, _peer.port);
}

std::tuple<uint32_t, uint32_t> SctpAssociationImpl::getTags() const
{
    return std::tuple<uint32_t, uint32_t>(_local.tag, _peer.tag);
}

void SctpAssociationImpl::onInitAck(const SctpPacket& packet,
    const InitAckChunk& initAckChunk,
    const uint64_t timestamp)
{
    if (_state != State::COOKIE_WAIT)
    {
        return; // collision, ignore it [rfc4960]
    }

    _peer.tag = initAckChunk.initTag;
    _peer.tsn = initAckChunk.initTSN;
    _local.cumulativeAck = _peer.tsn - 1;
    _peer.advertisedReceiveWindow = initAckChunk.advertisedReceiverWindow;

    auto params = initAckChunk.params();
    auto param = getParameter<ChunkParameter>(params, ChunkParameterType::StateCookie);
    if (param != nullptr)
    {
        if (param->dataSize() > _connect.echoedCookie.maxSize())
        {
            logger::error("Cookie received is too large!", _loggableId.c_str());
            setState(State::CLOSED);
            return;
        }

        SctpPacketW outboundPacket(_peer.tag, _local.port, _peer.port);
        auto& echoChunk = outboundPacket.appendChunk<CookieEchoChunk>();
        _connect.echoedCookie.set(*param);
        echoChunk.setCookie(*param);
        outboundPacket.commitAppendedChunk();
        _transport.send(outboundPacket);

        _connect.retransmitCount = 0;
        _connect.initTimer.stop();
        _connect.cookieTimer.startMs(timestamp, _config.init.timeout);
        setState(State::COOKIE_ECHOED);
        return;
    }
}

void SctpAssociationImpl::onCookieAckReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    if (_state == State::COOKIE_ECHOED)
    {
        setState(State::ESTABLISHED);
        _connect.cookieTimer.stop();
    }
    // discard in all other states
}

// cookies should be used to start up associations.
// This is anomaly condition that has to be resolved
void SctpAssociationImpl::onUnexpectedCookieEcho(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    auto* cookieChunk = sctpPacket.getChunk<CookieEchoChunk>(ChunkType::COOKIE_ECHO);
    if (cookieChunk)
    {
        auto& cookie = cookieChunk->getCookie<SctpCookie>();
        auto& header = sctpPacket.getHeader();
        if (cookie.tag.local == _local.tag && header.verificationTag == _local.tag && cookie.tag.peer != _peer.tag)
        {
            // (B) cookie from another setup previous to this one
            // embrace this new peer identity
            _peer.tag = cookie.tag.peer;
            setState(State::ESTABLISHED);
            SctpPacketW outPacket(_peer.tag, _local.port, _peer.port);
            outPacket.addChunk<GenericChunk>(ChunkType::COOKIE_ACK);
            _transport.send(outPacket);
        }
        else if (cookie.tag.local == _local.tag && cookie.tag.peer == _peer.tag && _state == State::COOKIE_ECHOED)
        {
            // (D) correct cookie
            setState(State::ESTABLISHED);
            SctpPacketW outPacket(_peer.tag, _local.port, _peer.port);
            outPacket.addChunk<GenericChunk>(ChunkType::COOKIE_ACK);
            _transport.send(outPacket);
        }
        else if (cookie.tag.local != _local.tag && cookie.tag.peer == _peer.tag && _local.tieTag == 0 &&
            _peer.tieTag == 0)
        {
            // (C) old/late cookie, discard
            return;
        }
        else if (cookie.tag.local != _local.tag && cookie.tag.peer != _peer.tag &&
            cookie.tieTag.local == _local.tieTag && cookie.tieTag.peer == _peer.tieTag)
        {
            // (A)restart, reset rwnd, etc enter ESTABLISHED
            // how did I receive this? remote sent INIT ?
        }
    }
    // rfc4960 action table
}

void SctpAssociationImpl::onUnexpectedInitReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    if (_state == State::COOKIE_WAIT || _state == State::COOKIE_ECHOED)
    {
        // respond with INIT_ACK with info from original INIT, but combine with new info
        // do not change internal state
        auto initChunk = sctpPacket.getChunk<InitChunk>(ChunkType::INIT);
        const uint32_t newPeerTag = initChunk->initTag;

        SctpPacketW initAckPacket(newPeerTag, _local.port, _peer.port);

        auto& initAck = initAckPacket.appendChunk<InitAckChunk>();
        initAck.inboundStreams = _local.inboundStreamCount;
        initAck.outboundStreams = _local.outboundStreamCount;
        initAck.initTag = _local.tag;
        initAck.initTSN = _local.tsn;
        initAck.advertisedReceiverWindow = _config.receiveWindow.initial;

        // combine with new info
        SctpCookie cookie(timestamp, _local.tag, newPeerTag, initChunk->initTSN, initChunk->advertisedReceiverWindow);
        cookie.inboundStreams = _local.inboundStreamCount;
        cookie.outboundStreams = _local.outboundStreamCount;
        if (_state == State::COOKIE_ECHOED)
        {
            _local.tieTag = _local.tag;
            cookie.tieTag.local = _local.tieTag;
            _peer.tieTag = _peer.tag;
            cookie.tieTag.peer = _peer.tieTag;
        }
        cookie.sign(_transport.getCurrentCookieSignKey(), _transport.getSignKeyLength(), _peer.port);
        initAck.add(CookieParameter<SctpCookie>(cookie));
        initAckPacket.commitAppendedChunk();

        _transport.send(initAckPacket);
    }
    else if (_state == State::SHUTDOWN_ACK_SENT)
    {
    }
    else if (_state == State::CLOSED)
    {
    }
    else
    {
        // send INIT_ACK populate tie-tags
        auto initChunk = sctpPacket.getChunk<InitChunk>(ChunkType::INIT);
        const uint32_t newPeerTag = initChunk->initTag;
        SctpPacketW initAckPacket(newPeerTag, _local.port, _peer.port);
        auto& initAck = initAckPacket.appendChunk<InitAckChunk>();
        initAck.inboundStreams = _local.inboundStreamCount;
        initAck.outboundStreams = _local.outboundStreamCount;
        initAck.initTag = _local.tag;
        initAck.initTSN = _local.tag;
        initAck.advertisedReceiverWindow = _config.receiveWindow.initial;

        // combine with new info
        SctpCookie cookie(timestamp, _local.tag, newPeerTag, initChunk->initTSN, initChunk->advertisedReceiverWindow);
        cookie.inboundStreams = _local.inboundStreamCount;
        cookie.outboundStreams = _local.outboundStreamCount;
        _local.tieTag = _local.tag;
        cookie.tieTag.local = _local.tieTag;
        _peer.tieTag = _peer.tag;
        cookie.tieTag.peer = _peer.tieTag;

        cookie.sign(_transport.getCurrentCookieSignKey(), _transport.getSignKeyLength(), _peer.port);
        initAck.add(CookieParameter<SctpCookie>(cookie));
        initAckPacket.commitAppendedChunk();
        _transport.send(initAckPacket);
    }
}

uint32_t SctpAssociationImpl::getFlightSize(uint64_t timestamp) const
{
    uint32_t count = 0;
    for (auto chunk : _outboundDataChunks)
    {
        if (chunk->transmitCount > 0 && diff(timestamp, chunk->transmitTime + _flow.getRetransmitTimeout()) > 0)
        {
            count += chunk->size;
        }
        else
        {
            break;
        }
    }
    return count;
}

size_t SctpAssociationImpl::outboundPendingSize() const
{
    size_t count = 0;
    for (auto chunk : _outboundDataChunks)
    {
        count += chunk->size;
    }
    return count;
}

namespace
{
template <typename T>
void appendPayloadData(SctpPacketW& packet, const T& chunk)
{
    auto& payloadChunk = packet.appendChunk<PayloadDataChunk>(chunk.streamId,
        chunk.streamSequenceNumber,
        chunk.payloadProtocol,
        chunk.transmissionSequenceNumber);
    payloadChunk.writeData(chunk.data(), chunk.size, chunk.fragmentBegin, chunk.fragmentEnd);
    packet.commitAppendedChunk();
}
} // namespace

void SctpAssociationImpl::processOutboundChunks(const uint64_t timestamp)
{
    const SelectiveAckChunk& pendingAck = reinterpret_cast<SelectiveAckChunk&>(_ack.pendingAck);
    const auto flightSize = getFlightSize(timestamp);
    const uint32_t windowSize = std::min(_flow.congestionWindow, _peer.advertisedReceiveWindow);
    uint32_t availableWindow = windowSize - std::min(windowSize, flightSize);

    if (_ack.prepared)
    {
        availableWindow -= std::min(static_cast<size_t>(availableWindow), pendingAck.size());
    }

    size_t burstLimit = std::max(1, _config.flow.maxBurst);
    if (_peer.advertisedReceiveWindow == 0 && flightSize == 0)
    {
        // condition for sending a probe packet in case inbound sack was lost
        availableWindow = _mtu.current;
        burstLimit = 1;
        for (auto* chunk : _outboundDataChunks)
        {
            if (chunk->transmitCount > 0)
            {
                chunk->transmitCount = 0;
            }
            else
            {
                break;
            }
        }
    }

    if (!_outboundDataChunks.empty() && availableWindow > 0)
    {
        SCTP_LOG("window avail:%d flight:%u cwnd:%u adv:%u chunks:%zu",
            _loggableId.c_str(),
            availableWindow,
            flightSize,
            _flow.congestionWindow,
            _peer.advertisedReceiveWindow,
            _outboundDataChunks.size());
    }

    uint8_t packetArea[_mtu.current + 16];
    SctpPacketW packet(_peer.tag, _local.port, _peer.port, packetArea, sizeof(packetArea));
    if (_ack.prepared)
    {
        packet.add(pendingAck);
        _ack.prepared = false;
    }

    uint32_t retransmitsCount = 0;
    for (auto it = _outboundDataChunks.begin();
         it != _outboundDataChunks.end() && burstLimit > 0 && availableWindow > 0;
         ++it)
    {
        auto& chunk = **it;
        if (packet.capacity() < chunk.fullSize())
        {
            _transport.send(packet);
            packet.clear();
            --burstLimit;
        }

        if (burstLimit == 0)
        {
            break;
        }
        else if (chunk.transmitCount == 0)
        {
            assert(packet.capacity() >= chunk.fullSize());
            appendPayloadData(packet, chunk);
            chunk.transmitTime = timestamp;
            ++chunk.transmitCount;
            availableWindow -= std::min(availableWindow, chunk.fullSize());
            if (it == _outboundDataChunks.begin())
            {
                if (_peer.advertisedReceiveWindow > 0)
                {
                    _flow.resetRetransmitTimeout();
                    _flow.retransmitTimer.expireAt(
                        _outboundDataChunks.front()->transmitTime + _flow.getRetransmitTimeout());
                }
                else
                {
                    _flow.doubleRetransmitTimeout();
                    _flow.retransmitTimer.startNs(timestamp, _flow.getRetransmitTimeout());
                }
            }
        }
        else if (diff(chunk.transmitTime + _flow.getRetransmitTimeout(), timestamp) >= 0)
        {
            assert(packet.capacity() >= chunk.fullSize());
            appendPayloadData(packet, chunk);
            chunk.transmitTime = timestamp;
            ++chunk.transmitCount;
            availableWindow -= std::min(availableWindow, chunk.fullSize());
            retransmitsCount += 1;
            logger::debug("retransmitted chunk %x, %u %u",
                _loggableId.c_str(),
                chunk.transmissionSequenceNumber,
                chunk.size,
                chunk.transmitCount);
        }
    }

    updateRetransmitTimer(timestamp, retransmitsCount > 0);

    if (packet.size() > SctpPacketW::HEADER_SIZE)
    {
        _transport.send(packet);
    }
}

void SctpAssociationImpl::updateRetransmitTimer(uint64_t timestamp, bool retransmitsPerformed)
{
    if (_outboundDataChunks.empty())
    {
        _flow.resetRetransmitTimeout();
        _flow.retransmitTimer.stop();
        _flow.idleTimer.startNs(timestamp, _rtt.getPeak());
    }
    else if (retransmitsPerformed)
    {
        _flow.doubleRetransmitTimeout();
        _flow.retransmitTimer.startNs(timestamp, _flow.getRetransmitTimeout());

        logger::debug("extended rto timer is %" PRIu64 " ms", "", _flow.getRetransmitTimeout() / timer::ms);
        if (_peer.advertisedReceiveWindow > 0)
        {
            _flow.onTransmitTimeout(timestamp, _mtu.current);
        }
    }
    else if (_flow.retransmitTimer.hasExpired(timestamp))
    {
        _flow.retransmitTimer.expireAt(timestamp + _flow.getRetransmitTimeout());
    }
}

void SctpAssociationImpl::onSackReceived(const SctpPacket& sctpPacket,
    const SelectiveAckChunk& ackChunk,
    const uint64_t timestamp)
{
    const auto cumulativeAck = ackChunk.cumulativeTsnAck.get();
    const auto tsnAdvance = diff(_peer.cumulativeAck, cumulativeAck);
    if (tsnAdvance < 0)
    {
        logger::debug("received old ack %x", "", cumulativeAck);
        return;
    }

    uint32_t bytesAcked = 0;
    auto unackedIt = _outboundDataChunks.begin();
    for (;
         unackedIt != _outboundDataChunks.end() && (diff((*unackedIt)->transmissionSequenceNumber, cumulativeAck) >= 0);
         ++unackedIt)
    {
        bytesAcked += (*unackedIt)->size;
        if ((*unackedIt)->transmitCount == 1)
        {
            _rtt.update(timestamp - (*unackedIt)->transmitTime);
        }
    }

    SCTP_LOG("sack received %x, advRwnd %u pending chunks %zu, hold %x, rtt %0.3f",
        _loggableId.c_str(),
        ackChunk.cumulativeTsnAck.get(),
        ackChunk.advertisedReceiverWindow.get(),
        _outboundDataChunks.size(),
        unackedIt != _outboundDataChunks.end() ? (*unackedIt)->transmissionSequenceNumber : 0,
        _rtt.getPeak() / timer::ms);

    _peer.cumulativeAck = cumulativeAck;

    auto flightSize = getFlightSize(timestamp);
    if (unackedIt != _outboundDataChunks.begin())
    {
        for (auto it = _outboundDataChunks.begin(); it != unackedIt; ++it)
        {
            _outboundBuffer.free(*it);
        }
        _outboundDataChunks.erase(_outboundDataChunks.begin(), unackedIt);
    }

    int lossCount = 0;
    int ackIndex = 0;
    int duplicateIndex = 0;
    for (auto it = _outboundDataChunks.begin(); it != _outboundDataChunks.end() &&
         (ackChunk.gapAckBlockCount > ackIndex || ackChunk.gapDuplicateCount > duplicateIndex);)
    {
        bool eraseItem = false;
        auto& outboundChunk = **it;
        for (; ackChunk.gapAckBlockCount > ackIndex; ++ackIndex)
        {
            auto ackBlock = ackChunk.getAck(ackIndex);
            if (diff(ackBlock.end, outboundChunk.transmissionSequenceNumber) > 0)
            {
                continue;
            }
            else if (diff(outboundChunk.transmissionSequenceNumber, ackBlock.start) > 0)
            {
                if (outboundChunk.transmitCount > 0)
                {
                    ++outboundChunk.nackCount;
                    if (outboundChunk.nackCount == 1)
                    {
                        ++lossCount;
                    }
                }
                break;
            }
            else // must be inside ack
            {
                if (outboundChunk.transmitCount == 1)
                {
                    _rtt.update(timestamp - outboundChunk.transmitTime);
                }
                bytesAcked += outboundChunk.size;
                eraseItem = true;
                SCTP_LOG("ack chunk inside block %x-%x", "", ackBlock.start, ackBlock.end);
                break;
            }
        }

        for (; ackChunk.gapDuplicateCount > duplicateIndex; ++duplicateIndex)
        {
            if (outboundChunk.transmissionSequenceNumber == ackChunk.getDuplicate(duplicateIndex))
            {
                eraseItem = true;
                SCTP_LOG("duplicate chunk %x", "", outboundChunk.transmissionSequenceNumber);
                break;
            }
            if (diff(outboundChunk.transmissionSequenceNumber, ackChunk.getDuplicate(duplicateIndex)) > 0)
            {
                break;
            }
        }
        if (eraseItem)
        {
            _outboundBuffer.free(*it);
            it = _outboundDataChunks.erase(it);
        }
        else
        {
            ++it;
        }
    }

    const auto advertisedWindow = ackChunk.advertisedReceiverWindow.get();
    _peer.advertisedReceiveWindow = advertisedWindow - std::min(advertisedWindow, bytesAcked);

    _flow.retransmitTimeout0 = _rtt.getPeak();

    if (lossCount > 0 && tsnAdvance > 0)
    {
        _flow.onPacketLoss(timestamp, _mtu.current);
    }

    if (tsnAdvance > 0)
    {
        _flow.onSackReceived(timestamp, _mtu.current, flightSize, bytesAcked, _outboundDataChunks.empty());
    }

    if (_outboundDataChunks.empty() ||
        (_outboundDataChunks.front()->transmitCount == 0 && _peer.advertisedReceiveWindow > 0))
    {
        _flow.retransmitTimer.stop();
    }
    handleFastRetransmits(timestamp);

    processOutboundChunks(timestamp);
}

void SctpAssociationImpl::handleFastRetransmits(uint64_t timestamp)
{
    if (!_flow.inFastRecovery)
    {
        for (auto* chunk : _outboundDataChunks)
        {
            if (chunk->nackCount == 3 && !_flow.inFastRecovery)
            {
                _flow.inFastRecovery = true;
                logger::debug("entering fast recovery", _loggableId.c_str());
            }

            if (chunk->transmitCount > 0)
            {
                _flow.fastRecoveryExitPoint = chunk->transmissionSequenceNumber;
            }
            else if (chunk->transmitCount == 0)
            {
                break;
            }
        }
    }
    else
    {
        if (_outboundDataChunks.empty() ||
            diff(_flow.fastRecoveryExitPoint, _outboundDataChunks.front()->transmissionSequenceNumber) > 0)
        {
            _flow.inFastRecovery = false;
        }
    }

    if (_flow.inFastRecovery)
    {
        uint8_t packetArea[_mtu.current + 16];
        SctpPacketW packet(_peer.tag, _local.port, _peer.port, packetArea, sizeof(packetArea));

        for (auto* chunk : _outboundDataChunks)
        {
            if (chunk->nackCount == 3 && packet.capacity() > chunk->fullSize())
            {
                ++chunk->nackCount;
                appendPayloadData(packet, *chunk);
                chunk->transmitTime = timestamp;
                ++chunk->transmitCount;
                break;
            }
            else if (chunk->nackCount == 3)
            {
                ++chunk->nackCount; // do not fast retransmit if it did not fit into single packet
            }
        }

        if (packet.size() > SctpPacketW::HEADER_SIZE)
        {
            _transport.send(packet);
        }
    }
}

// SACK will always be prepared if this method is called.
// Have a guard prior to calling if sack is conditional
void SctpAssociationImpl::prepareSack(const uint64_t timestamp)
{
    auto it = _inboundDataChunks.begin();
    for (;
         it != _inboundDataChunks.end() && diff(it->second->transmissionSequenceNumber, _local.cumulativeAck + 1) >= 0;
         ++it)
    {
        if (it->second->transmissionSequenceNumber == _local.cumulativeAck + 1)
        {
            ++_local.cumulativeAck;
        }
    }
    SackBuilder sackBuilder(_ack.pendingAck,
        std::min(sizeof(_ack.pendingAck), _mtu.current - SctpPacketW::HEADER_SIZE - SelectiveAckChunk::HEADER_SIZE));

    sackBuilder.ack.cumulativeTsnAck = _local.cumulativeAck;
    SCTP_LOG("ack up to %x", _loggableId.c_str(), _local.cumulativeAck);

    // ack blocks we have received
    if (it != _inboundDataChunks.end())
    {
        uint32_t blockStart = it->second->transmissionSequenceNumber;
        uint32_t nextTsn = it->second->transmissionSequenceNumber + 1;
        uint32_t blockEnd = nextTsn;
        for (++it; it != _inboundDataChunks.end(); ++it)
        {
            if (it->second->transmissionSequenceNumber != nextTsn)
            {
                sackBuilder.addAck(blockStart, blockEnd);
                blockStart = it->second->transmissionSequenceNumber;
            }

            ++nextTsn;
            blockEnd = nextTsn;
        }
        sackBuilder.addAck(blockStart, blockEnd);
    }

    sackBuilder.ack.advertisedReceiverWindow = _local.advertisedReceiveWindow;
    _ack.prepared = true;
}

// gaps in data TODO continue and find complete fragments that do not require in order
// This will require more elaborate discarding of data in the inbound buffer.
bool SctpAssociationImpl::gatherReceivedFragments(const uint64_t timestamp)
{
    if (_inboundDataChunks.empty())
    {
        return false;
    }

    InboundChunkList::iterator itBegin = _inboundDataChunks.end();
    size_t fragmentSize = 0;
    for (auto it = _inboundDataChunks.begin();
         it != _inboundDataChunks.end() && diff(it->second->transmissionSequenceNumber, _local.cumulativeAck) >= 0;
         ++it)
    {
        auto& chunk = *it->second;

        if (chunk.fragmentBegin)
        {
            fragmentSize = 0;
            itBegin = it;
        }
        fragmentSize += chunk.size;

        if (chunk.fragmentEnd && itBegin != _inboundDataChunks.end())
        {
            reportFragment(itBegin, fragmentSize, timestamp);
            return true;
        }
    }
    return false;
}

void SctpAssociationImpl::reportFragment(InboundChunkList::iterator chunkHeadIt,
    const size_t fragmentSize,
    const uint64_t timestamp)
{
    memory::Array<uint8_t, 512> buffer(fragmentSize);

    ReceivedDataChunk chunkHead = *chunkHeadIt->second;
    if (_streams.find(chunkHead.streamId) == _streams.cend())
    {
        auto pairIt = _streams.emplace(std::forward_as_tuple(chunkHead.streamId, chunkHead.streamId));
        pairIt.first->second.sequenceCounter = chunkHead.streamSequenceNumber;
    }

    for (auto it = chunkHeadIt; it != _inboundDataChunks.end(); ++it)
    {
        const bool isFragmentEnd = it->second->fragmentEnd;
        buffer.append(it->second->data(), it->second->size);
        _inboundBuffer.free(it->second);
        if (isFragmentEnd)
        {
            _inboundDataChunks.erase(chunkHeadIt, ++it);
            break;
        }
    }

    _listener->onSctpFragmentReceived(this,
        chunkHead.streamId,
        chunkHead.streamSequenceNumber,
        chunkHead.payloadProtocol,
        buffer.data(),
        fragmentSize,
        timestamp);
}

void SctpAssociationImpl::onAbortReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    auto* chunk = sctpPacket.getChunk<AbortChunk>(ChunkType::ABORT);
    if (chunk)
    {
        logger::debug("abort chunk received", _loggableId.c_str());

        for (auto& cause : chunk->causes())
        {
            if (cause.getCause() == ErrorCause::ProtocolError)
            {
                auto description = std::string(reinterpret_cast<const char*>(cause.data()), cause.dataSize());
                logger::warn("SCTP abort received %s", _loggableId.c_str(), description.c_str());
            }
            else
            {
                logger::warn("SCTP abort cause code %u %s",
                    _loggableId.c_str(),
                    cause.getCause(),
                    toString(cause.getCause()));
            }
        }
    }
    _listener->onSctpClosed(this);
}

void SctpAssociationImpl::onErrorReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    logger::error("SCTP error received", _loggableId.c_str());
}
void SctpAssociationImpl::onShutDownReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    logger::info("SCTP shutdown received", _loggableId.c_str());
}

void SctpAssociationImpl::onShutDownAckReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    logger::error("SCTP shutdown ack received", _loggableId.c_str());
}
void SctpAssociationImpl::onShutDownCompleteReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    logger::error("SCTP shutdown complete received", _loggableId.c_str());
}

void SctpAssociationImpl::onHeartbeatRequest(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    auto* heartbeatChunk = sctpPacket.getChunk(ChunkType::HEARTBEAT);

    SctpPacketW response(_peer.tag, _local.port, _peer.port);
    auto& heartbeatAck = response.appendChunk<GenericChunk>(ChunkType::HEARTBEAT_ACK);

    auto params = heartbeatChunk->params();
    auto param = getParameter(params, ChunkParameterType::HeartbeatInfo);
    if (param)
    {
        heartbeatAck.add(*param);
        response.commitAppendedChunk();
        _transport.send(response);
    }
}

void SctpAssociationImpl::onHeartbeatResponse(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    auto* heartbeatChunk = sctpPacket.getChunk(ChunkType::HEARTBEAT_ACK);
    auto params = heartbeatChunk->params();
    auto* info = getParameter<HeartbeatInfoParameter>(params, ChunkParameterType::HeartbeatInfo);
    if (info)
    {
        auto rtt = timestamp - info->timestamp;
        _rtt.update(rtt);
        _flow.retransmitTimeout0 = _rtt.getPeak();
        if (info->mtu > _mtu.current)
        {
            _mtu.current = info->mtu;
        }

        if (_mtu.selectedProbe + 1 != std::end(_mtu.MTU_SIZES))
        {
            ++_mtu.selectedProbe;
        }
        if (*_mtu.selectedProbe > _mtu.current)
        {
            sendMtuProbe(timestamp);
        }
        else
        {
            _mtu.probing = false;
            _mtu.probeTimer.stop();
        }
    }
}

void SctpAssociationImpl::onDataReceived(const SctpPacket& sctpPacket, const uint64_t timestamp)
{
    bool dataReceived = false;
    bool newDataReceived = false; // indicates timeout on peer side for ACK

    uint32_t lastReceivedTsn;
    for (auto& chunk : sctpPacket.chunks())
    {
        if (chunk.header.type == ChunkType::SACK)
        {
            onSackReceived(sctpPacket, reinterpret_cast<const SelectiveAckChunk&>(chunk), timestamp);
        }
        else if (chunk.header.type == ChunkType::DATA)
        {
            dataReceived = true;
            auto& payloadChunk = reinterpret_cast<const PayloadDataChunk&>(chunk);
            lastReceivedTsn = payloadChunk.transmissionSequenceNumber;
            if (payloadChunk.payloadSize() > 0)
            {
                const uint32_t tsn = payloadChunk.transmissionSequenceNumber;

                auto chunkIt = _inboundDataChunks.find(tsn);
                if (chunkIt != _inboundDataChunks.end())
                {
                    ++chunkIt->second->receiveCount;
                    SCTP_LOG("duplicate data chunk %x received", "", tsn);
                    continue;
                }
                else if (diff(tsn, _local.cumulativeAck) >= 0)
                {
                    // we already acked this range and removed the chunk from inbound list
                    SCTP_LOG("duplicate data chunk %x received", "", tsn);
                    continue;
                }

                if (_local.advertisedReceiveWindow > 0)
                {
                    ReceivedDataChunk* receivedChunk =
                        _inboundBuffer.instantiate<ReceivedDataChunk>(payloadChunk.payloadSize(),
                            payloadChunk,
                            timestamp);
                    if (receivedChunk)
                    {
                        _inboundDataChunks[tsn] = receivedChunk;
                        if (diff(_peer.tsn, tsn) >= 0)
                        {
                            _peer.tsn = tsn + 1;
                        }
                        newDataReceived = true;
                    }
                    else
                    {
                        logger::error("SCTP receive buffer depleted. Dropped chunk %zuB",
                            _loggableId.c_str(),
                            payloadChunk.payloadSize());
                    }
                }
            }
            else if (payloadChunk.payloadSize() == 0)
            {
                logger::error("empty DATA CHUNK received %x", _loggableId.c_str(), lastReceivedTsn);
                CauseCode code(ErrorCause::NoUserData, lastReceivedTsn);
                sendErrorResponse(sctpPacket, code);
                if (_listener)
                {
                    _listener->onSctpClosed(this);
                }
            }
        }
    }

    if (dataReceived)
    {
        prepareSack(timestamp);

        while (newDataReceived && gatherReceivedFragments(timestamp))
            ;

        processOutboundChunks(timestamp);
    }
}

void SctpAssociationImpl::close()
{
    _state = State::CLOSED;
}

std::unique_ptr<SctpAssociation> createSctpAssociation(size_t logId,
    SctpServerPort& transport,
    uint16_t remotePort,
    SctpAssociation::IEvents* listener,
    const SctpConfig& config)
{
    return std::make_unique<SctpAssociationImpl>(logId, transport, remotePort, listener, config);
}

std::unique_ptr<SctpAssociation> createSctpAssociation(size_t logId,
    SctpServerPort& transport,
    const SctpPacket& cookieEcho,
    SctpAssociation::IEvents* listener,
    const SctpConfig& config)
{
    return std::make_unique<SctpAssociationImpl>(logId, transport, cookieEcho, listener, config);
}
} // namespace sctp
