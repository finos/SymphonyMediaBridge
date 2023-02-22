#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/RtpVideoRewriter.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

VideoForwarderRtxReceiveJob::VideoForwarderRtxReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& rtxSsrcContext,
    bridge::SsrcInboundContext& ssrcContext,
    const uint32_t mainSsrc,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _engineMixer(engineMixer),
      _sender(sender),
      _rtxSsrcContext(rtxSsrcContext),
      _ssrcContext(ssrcContext),
      _mainSsrc(mainSsrc),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
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

    if (!_ssrcContext.videoMissingPacketsTracker)
    {
        return;
    }

    const auto oldRolloverCounter = _rtxSsrcContext.lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u", "VideoForwarderRtxReceiveJob", _rtxSsrcContext.ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_rtxSsrcContext.ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "VideoForwarderReceiveJob",
                _rtxSsrcContext.ssrc,
                _engineMixer.getLoggableId().c_str());
            return;
        }
    }

    if (!_sender->unprotect(*_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "VideoForwarderRtxReceiveJob",
            _rtxSsrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        return;
    }

    _rtxSsrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    RtpVideoRewriter::rewriteRtxPacket(*_packet,
        _mainSsrc,
        _ssrcContext.rtpMap.payloadType,
        _sender->getLoggableId().c_str());

    // Missing packet tracker will know which extended sequence number this packet is referring to and if this packet
    // has already been received and forwarded.
    uint32_t extendedSequenceNumber = 0;
    if (!_ssrcContext.videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber.get(),
            extendedSequenceNumber))
    {
        return;
    }

    _engineMixer.onForwarderVideoRtpPacketDecrypted(_ssrcContext, std::move(_packet), extendedSequenceNumber);
}

} // namespace bridge
