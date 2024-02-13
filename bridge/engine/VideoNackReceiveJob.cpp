#include "bridge/engine/VideoNackReceiveJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

#define NACK_LOG(fmt, ...) // logger::debug(fmt, ##__VA_ARGS__)

namespace bridge
{

VideoNackReceiveJob::VideoNackReceiveJob(SsrcOutboundContext& rtxSsrcOutboundContext,
    transport::RtcTransport& sender,
    SsrcOutboundContext& mainOutboundContext,
    const uint16_t pid,
    const uint16_t blp,
    const uint64_t timestamp,
    const uint64_t rtt)
    : jobmanager::CountedJob(sender.getJobCounter()),
      _rtxSsrcOutboundContext(rtxSsrcOutboundContext),
      _sender(sender),
      _mainOutboundContext(mainOutboundContext),
      _pid(pid),
      _blp(blp),
      _timestamp(timestamp),
      _rtt(rtt)
{
}

void VideoNackReceiveJob::run()
{
    NACK_LOG("Incoming rtcp feedback NACK, rtxSsrc %u, pid %u, blp 0x%x, %s",
        "VideoNackReceiveJob",
        _rtxSsrcOutboundContext.ssrc.get(),
        _pid,
        _blp,
        _sender.getLoggableId().c_str());

    // it may be that we post a few of these jobs before cache has been set, but that is ok
    if (!_sender.isConnected() || !_mainOutboundContext.packetCache.isSet() || !_mainOutboundContext.packetCache.get())
    {
        return;
    }

    if (_pid == _rtxSsrcOutboundContext.lastRespondedNackPid && _blp == _rtxSsrcOutboundContext.lastRespondedNackBlp &&
        _timestamp - _rtxSsrcOutboundContext.lastRespondedNackTimestamp < _rtt)
    {
        NACK_LOG("Ignoring NACK, rtxSsrc %u, pid %u, blp 0x%x, time since last response %" PRIi64
                 " us less than rtt %" PRIi64 "us",
            "VideoNackReceiveJob",
            _rtxSsrcOutboundContext.ssrc.get(),
            _pid,
            _blp,
            (_timestamp - _rtxSsrcOutboundContext.lastRespondedNackTimestamp) / utils::Time::us,
            _rtt / utils::Time::us);
        return;
    }

    _rtxSsrcOutboundContext.lastRespondedNackPid = _pid;
    _rtxSsrcOutboundContext.lastRespondedNackBlp = _blp;
    _rtxSsrcOutboundContext.lastRespondedNackTimestamp = _timestamp;

    auto sequenceNumber = _pid;
    sendIfCached(sequenceNumber);

    auto blp = _blp;
    while (blp != 0)
    {
        ++sequenceNumber;

        if ((blp & 0x1) == 0x1)
        {
            sendIfCached(sequenceNumber);
        }

        blp = blp >> 1;
    }
}

void VideoNackReceiveJob::sendIfCached(const uint16_t sequenceNumber)
{
    if (static_cast<int16_t>((_rtxSsrcOutboundContext.lastKeyFrameSequenceNumber & 0xFFFFu) - sequenceNumber) > 0)
    {
        NACK_LOG("%u ignoring NACK for pre key frame packet %u, key frame at %u",
            "VideoNackReceiveJob",
            _rtxSsrcOutboundContext.ssrc.get(),
            sequenceNumber,
            _rtxSsrcOutboundContext.lastKeyFrameSequenceNumber);
        return;
    }

    const auto cachedPacket = _mainOutboundContext.packetCache.get()->get(sequenceNumber);
    if (!cachedPacket)
    {
        return;
    }

    const auto cachedRtpHeader = rtp::RtpHeader::fromPacket(*cachedPacket);
    if (!cachedRtpHeader)
    {
        return;
    }

    auto packet = memory::makeUniquePacket(_rtxSsrcOutboundContext.allocator);
    if (!packet)
    {
        return;
    }

    const auto cachedRtpHeaderLength = cachedRtpHeader->headerLength();
    const auto cachedPayload = cachedRtpHeader->getPayload();
    const auto cachedSequenceNumber = cachedRtpHeader->sequenceNumber.get();

    memcpy(packet->get(), cachedPacket->get(), cachedRtpHeaderLength);
    auto copyHead = packet->get() + cachedRtpHeaderLength;
    reinterpret_cast<uint16_t*>(copyHead)[0] = hton<uint16_t>(cachedSequenceNumber);
    copyHead += sizeof(uint16_t);
    memcpy(copyHead, cachedPayload, cachedPacket->getLength() - cachedRtpHeaderLength);
    packet->setLength(cachedPacket->getLength() + sizeof(uint16_t));

    NACK_LOG("Sending cached packet seq %u, rtxSsrc %u, seq %u",
        "VideoNackReceiveJob",
        sequenceNumber,
        _rtxSsrcOutboundContext.ssrc.get(),
        _rtxSsrcOutboundContext.sequenceCounter & 0xFFFFu);

    auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        return;
    }

    rtpHeader->ssrc = _rtxSsrcOutboundContext.ssrc;
    rtpHeader->payloadType = _rtxSsrcOutboundContext.rtpMap.payloadType;
    rtpHeader->sequenceNumber = ++_rtxSsrcOutboundContext.getSequenceNumberReference() & 0xFFFF;

    _sender.protectAndSend(std::move(packet));
}

} // namespace bridge
