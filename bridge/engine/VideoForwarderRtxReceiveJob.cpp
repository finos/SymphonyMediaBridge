#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

#define DEBUG_REWRITER 0
#if DEBUG_REWRITER
#define REWRITER_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define REWRITER_LOG(fmt, ...) void(0)
#endif

namespace bridge
{

VideoForwarderRtxReceiveJob::VideoForwarderRtxReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& rtxSsrcContext,
    bridge::SsrcInboundContext& mainSsrcContext,
    const uint32_t mainSsrc,
    const uint32_t extendedSequenceNumber)
    : RtpForwarderReceiveBaseJob(std::move(packet), sender, engineMixer, rtxSsrcContext, extendedSequenceNumber),
      _mainSsrcContext(mainSsrcContext),
      _mainSsrc(mainSsrc)
{
}

void VideoForwarderRtxReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        return;
    }

    // RTX packets with padding flag set are empty padding packets for bandwidth estimation purposes, drop early.
    if (rtpHeader->padding == 1)
    {
        return;
    }

    if (!_mainSsrcContext.videoMissingPacketsTracker)
    {
        return;
    }

    if (!tryUnprotectRtpPacket("VideoForwarderRtxReceiveJob"))
    {
        return;
    }

    const auto headerLength = rtpHeader->headerLength();
    const auto payload = rtpHeader->getPayload();

    const auto originalSequenceNumber =
        (static_cast<uint16_t>(payload[0]) << 8) | (static_cast<uint16_t>(payload[1]) & 0xFF);

    memmove(payload, payload + sizeof(uint16_t), _packet->getLength() - headerLength - sizeof(uint16_t));
    _packet->setLength(_packet->getLength() - sizeof(uint16_t));

    REWRITER_LOG("%s rewriteRtxPacket ssrc %u -> %u, seq %u -> %u",
        "VideoForwarderRtxReceiveJob",
        transportName,
        rtpHeader->ssrc.get(),
        mainSsrc,
        rtpHeader->sequenceNumber.get(),
        originalSequenceNumber);

    rtpHeader->sequenceNumber = originalSequenceNumber;
    rtpHeader->ssrc = _mainSsrc;
    rtpHeader->payloadType = _mainSsrcContext.rtpMap.payloadType;

    // Missing packet tracker will know which extended sequence number this packet is referring to and if this packet
    // has already been received and forwarded.
    uint32_t extendedSequenceNumber = 0;
    if (!_mainSsrcContext.videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber.get(),
            extendedSequenceNumber))
    {
        return;
    }

    _engineMixer.onForwarderVideoRtpPacketDecrypted(_mainSsrcContext, std::move(_packet), extendedSequenceNumber);
}

} // namespace bridge
