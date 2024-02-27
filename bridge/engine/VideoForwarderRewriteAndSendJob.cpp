#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/H264Header.h"
#include "codec/Vp8Header.h"
#include "transport/Transport.h"
#include "utils/Function.h"

namespace bridge
{

VideoForwarderRewriteAndSendJob::VideoForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    SsrcInboundContext& senderInboundContext,
    memory::UniquePacket packet,
    transport::Transport& transport,
    const uint32_t extendedSequenceNumber,
    MixerManagerAsync& mixerManager,
    size_t endpointIdHash,
    EngineMixer& mixer,
    uint64_t timestamp)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _senderInboundContext(senderInboundContext),
      _packet(std::move(packet)),
      _transport(transport),
      _extendedSequenceNumber(extendedSequenceNumber),
      _mixerManager(mixerManager),
      _endpointIdHash(endpointIdHash),
      _mixer(mixer),
      _timestamp(timestamp)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void VideoForwarderRewriteAndSendJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        assert(false); // should have been checked multiple times
        return;
    }

    if (_outboundContext.rtpMap.format == RtpMap::Format::RTX)
    {
        logger::warn("%s rtx packet should not reach rewrite and send. ssrc %u, seq %u",
            "VideoForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            rtpHeader->ssrc.get(),
            _extendedSequenceNumber);
        return;
    }

    if (!_outboundContext.packetCache.isSet())
    {
        logger::debug("New ssrc %u seen on %s, sending request to add videoPacketCache to transport",
            "VideoForwarderRewriteAndSendJob",
            _outboundContext.ssrc,
            _transport.getLoggableId().c_str());

        _outboundContext.packetCache.set(nullptr);
        _mixerManager.asyncAllocateVideoPacketCache(_mixer, _outboundContext.ssrc, _endpointIdHash);
    }

    const auto payloadSize = _packet->getLength() - rtpHeader->headerLength();
    const auto payload = rtpHeader->getPayload();
    const auto isKeyFrame = _outboundContext.rtpMap.format == RtpMap::Format::H264
        ? codec::H264Header::isKeyFrame(payload, payloadSize)
        : codec::Vp8Header::isKeyFrame(payload, codec::Vp8Header::getPayloadDescriptorSize(payload, payloadSize));

    const auto ssrc = rtpHeader->ssrc.get();
    if (ssrc != _outboundContext.getOriginalSsrc())
    {
        if (!isKeyFrame)
        {
            _outboundContext.needsKeyframe = true;
            _senderInboundContext.pliScheduler.triggerPli();
        }
    }

    if (_outboundContext.needsKeyframe)
    {
        if (!isKeyFrame)
        {
            // dropping P-frames until key frame appears
            return;
        }

        _outboundContext.needsKeyframe = false;

        logger::debug("%s requested key frame from %u on ssrc %u",
            "VideoForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            _senderInboundContext.ssrc,
            _outboundContext.ssrc);
    }

    if (!_transport.isConnected())
    {
        return;
    }

    uint32_t rewrittenExtendedSequenceNumber = 0;

    if (!_outboundContext.rewriteVideo(*rtpHeader,
            _senderInboundContext,
            _extendedSequenceNumber,
            _transport.getLoggableId().c_str(),
            rewrittenExtendedSequenceNumber,
            _timestamp,
            isKeyFrame))
    {
        logger::info("%s dropping packet. Rewrite not suitable ssrc %u, seq %u",
            "VideoForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            rtpHeader->ssrc.get(),
            _extendedSequenceNumber);

        return;
    }

    if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
    {
        if (!_outboundContext.packetCache.get()->add(*_packet, rtpHeader->sequenceNumber))
        {
            logger::warn("%s failed to add packet to cache. ssrc %u, seq %u",
                "VideoForwarderRewriteAndSendJob",
                _transport.getLoggableId().c_str(),
                rtpHeader->ssrc.get(),
                rtpHeader->sequenceNumber.get());
        }
    }

    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
