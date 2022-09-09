#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/Vp8Rewriter.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

VideoForwarderRtxReceiveJob::VideoForwarderRtxReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcFeedbackContext,
    bridge::SsrcInboundContext& ssrcContext,
    const uint32_t mainSsrc,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcFeedbackContext(ssrcFeedbackContext),
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

    const auto oldRolloverCounter = _ssrcFeedbackContext.lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u",
            "VideoForwarderRtxReceiveJob",
            _ssrcFeedbackContext.ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcFeedbackContext.ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "VideoForwarderReceiveJob",
                _ssrcFeedbackContext.ssrc,
                _engineMixer.getLoggableId().c_str());
            return;
        }
    }

    if (!_sender->unprotect(*_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "VideoForwarderRtxReceiveJob",
            _ssrcFeedbackContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        return;
    }

    _ssrcFeedbackContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    Vp8Rewriter::rewriteRtxPacket(*_packet, _mainSsrc);

    if (!_ssrcFeedbackContext.videoMissingPacketsTracker.get())
    {
        assert(false);
        return;
    }

    uint32_t extendedSequenceNumber = 0;
    if (!_ssrcFeedbackContext.videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber.get(),
            extendedSequenceNumber))
    {
        return;
    }

    _engineMixer.onForwarderVideoRtpPacketDecrypted(_ssrcContext, std::move(_packet), extendedSequenceNumber);
}

} // namespace bridge
