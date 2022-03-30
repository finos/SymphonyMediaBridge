#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/Vp8Rewriter.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

VideoForwarderRtxReceiveJob::VideoForwarderRtxReceiveJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    const uint32_t mainSsrc,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(packet),
      _allocator(allocator),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _mainSsrc(mainSsrc),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

VideoForwarderRtxReceiveJob::~VideoForwarderRtxReceiveJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}

void VideoForwarderRtxReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    // RTX packets with padding flag set are empty padding packets for bandwidth estimation purposes, drop early.
    if (rtpHeader->padding == 1)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    const auto oldRolloverCounter = _ssrcContext._lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u", "VideoForwarderRtxReceiveJob", _ssrcContext._ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext._ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "VideoForwarderReceiveJob",
                _ssrcContext._ssrc,
                _engineMixer.getLoggableId().c_str());
            _allocator.free(_packet);
            _packet = nullptr;
            return;
        }
    }

    if (!_sender->unprotect(_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "VideoForwarderRtxReceiveJob",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    _ssrcContext._lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    Vp8Rewriter::rewriteRtxPacket(*_packet, _mainSsrc);

    if (!_ssrcContext._videoMissingPacketsTracker.get())
    {
        assert(false);
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    uint32_t extendedSequenceNumber = 0;
    if (!_ssrcContext._videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber.get(),
            extendedSequenceNumber))
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    _engineMixer.onForwarderVideoRtpPacketDecrypted(_sender, _packet, _allocator, extendedSequenceNumber);
    _packet = nullptr;
}

} // namespace bridge
