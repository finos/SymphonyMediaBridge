#include "bridge/engine/VideoNackReceiveJob.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "logger/Logger.h"
#include "memory/RefCountedPacket.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

#define NACK_LOG(fmt, ...) // logger::debug(fmt, ##__VA_ARGS__)

namespace bridge
{

VideoNackReceiveJob::VideoNackReceiveJob(SsrcOutboundContext& ssrcOutboundContext,
    transport::RtcTransport& sender,
    PacketCache& videoPacketCache,
    const uint16_t pid,
    const uint16_t blp,
    const uint32_t feedbackSsrc)
    : jobmanager::CountedJob(sender.getJobCounter()),
      _ssrcOutboundContext(ssrcOutboundContext),
      _sender(sender),
      _videoPacketCache(videoPacketCache),
      _pid(pid),
      _blp(blp),
      _feedbackSsrc(feedbackSsrc)
{
}

void VideoNackReceiveJob::run()
{
    NACK_LOG("Incoming rtcp feedback NACK, feedbackSsrc %u, pid %u, blp 0x%x",
        "VideoNackReceiveJob",
        _feedbackSsrc,
        _pid,
        _blp);

    if (!_sender.isConnected())
    {
        return;
    }

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
    auto scopedRef = memory::RefCountedPacket::ScopedRef(_videoPacketCache.get(sequenceNumber));
    if (!scopedRef._refCountedPacket)
    {
        return;
    }
    auto cachedPacket = scopedRef._refCountedPacket->get();
    if (!cachedPacket)
    {
        return;
    }

    const auto cachedRtpHeader = rtp::RtpHeader::fromPacket(*cachedPacket);
    if (!cachedRtpHeader)
    {
        return;
    }

    auto packet = memory::makePacket(_ssrcOutboundContext._allocator);
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

    NACK_LOG("Sending cached packet seq %u, feedbackSsrc %u, seq %u",
        "VideoNackReceiveJob",
        sequenceNumber,
        _feedbackSsrc,
        _ssrcOutboundContext._sequenceCounter);

    auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        _ssrcOutboundContext._allocator.free(packet);
        return;
    }

    rtpHeader->ssrc = _feedbackSsrc;
    rtpHeader->payloadType = static_cast<uint16_t>(RtpMap::Format::VP8RTX);
    rtpHeader->sequenceNumber = _ssrcOutboundContext._sequenceCounter & 0xFFFF;
    ++_ssrcOutboundContext._sequenceCounter;

    _sender.protectAndSend(packet, _ssrcOutboundContext._allocator);
}

} // namespace bridge
