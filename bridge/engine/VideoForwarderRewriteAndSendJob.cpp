#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "bridge/engine/Vp8Rewriter.h"
#include "transport/Transport.h"
#include "utils/OutboundSequenceNumber.h"

namespace
{

inline void rewriteHeaderExtensions(rtp::RtpHeader* rtpHeader,
    const bridge::SsrcInboundContext& senderInboundContext,
    const bridge::SsrcOutboundContext& receiverOutboundContext)
{
    assert(rtpHeader);

    const auto headerExtensions = rtpHeader->getExtensionHeader();
    if (!headerExtensions)
    {
        return;
    }

    const bool senderHasAbsSendTimeEx = senderInboundContext.rtpMap.absSendTimeExtId.isSet();
    const bool receiverHasAbsSendTimeEx = receiverOutboundContext.rtpMap.absSendTimeExtId.isSet();
    const bool absSendTimeExNeedToBeRewritten = senderHasAbsSendTimeEx && receiverHasAbsSendTimeEx &&
        senderInboundContext.rtpMap.absSendTimeExtId.get() != receiverOutboundContext.rtpMap.absSendTimeExtId.get();

    if (absSendTimeExNeedToBeRewritten)
    {
        for (auto& rtpHeaderExtension : headerExtensions->extensions())
        {
            if (rtpHeaderExtension.getId() == senderInboundContext.rtpMap.absSendTimeExtId.get())
            {
                rtpHeaderExtension.setId(receiverOutboundContext.rtpMap.absSendTimeExtId.get());
                return;
            }
        }
    }
}

} // namespace

namespace bridge
{

VideoForwarderRewriteAndSendJob::VideoForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    SsrcInboundContext& senderInboundContext,
    memory::UniquePacket packet,
    transport::Transport& transport,
    const uint32_t extendedSequenceNumber)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _senderInboundContext(senderInboundContext),
      _packet(std::move(packet)),
      _transport(transport),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void VideoForwarderRewriteAndSendJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        return;
    }

    bool isRetransmittedPacket = false;
    if (_outboundContext.rtpMap.format == RtpMap::Format::VP8RTX)
    {
        isRetransmittedPacket = true;
    }

    const bool isKeyFrame = codec::Vp8Header::isKeyFrame(rtpHeader->getPayload(),
        codec::Vp8Header::getPayloadDescriptorSize(rtpHeader->getPayload(),
            _packet->getLength() - rtpHeader->headerLength()));

    const auto ssrc = rtpHeader->ssrc.get();
    if (ssrc != _outboundContext.lastRewrittenSsrc)
    {
        if (isRetransmittedPacket)
        {
            return;
        }
        if (!isKeyFrame)
        {
            _outboundContext.needsKeyframe = true;
            _senderInboundContext.pliScheduler.triggerPli();
            if (utils::Time::diffGE(_outboundContext.pli.triggerTimestamp, _outboundContext.pli.keyFrameTimestamp, 0))
            {
                _outboundContext.pli.triggerTimestamp = utils::Time::getAbsoluteTime();
            }
        }
    }

    if (_outboundContext.needsKeyframe)
    {
        if (!isKeyFrame)
        {
            return;
        }
        else
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            const auto delay = _outboundContext.pli.getDelay(timestamp);
            _outboundContext.pli.keyFrameTimestamp = timestamp;
            _outboundContext.needsKeyframe = false;

            logger::debug("%s requested key frame from %u fwd after %" PRIu64 "ms",
                "VideoForwarderRewriteAndSendJob",
                _transport.getLoggableId().c_str(),
                _senderInboundContext.ssrc,
                delay / utils::Time::ms);
        }
    }

    uint32_t rewrittenExtendedSequenceNumber = 0;
    if (!Vp8Rewriter::rewrite(_outboundContext,
            *_packet,
            _outboundContext.ssrc,
            _extendedSequenceNumber,
            _transport.getLoggableId().c_str(),
            rewrittenExtendedSequenceNumber))
    {
        return;
    }

    uint16_t nextSequenceNumber;
    if (!utils::OutboundSequenceNumber::process(rewrittenExtendedSequenceNumber,
            _outboundContext.highestSeenExtendedSequenceNumber,
            _outboundContext.sequenceCounter,
            nextSequenceNumber))
    {
        return;
    }
    rtpHeader->sequenceNumber = nextSequenceNumber;
    if (isKeyFrame)
    {
        _outboundContext.lastKeyFrameSequenceNumber = nextSequenceNumber;
    }
    rtpHeader->payloadType = _outboundContext.rtpMap.payloadType;
    rewriteHeaderExtensions(rtpHeader, _senderInboundContext, _outboundContext);

    if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
    {
        if (!_outboundContext.packetCache.get()->add(*_packet, nextSequenceNumber))
        {
            return;
        }
    }

    if (!_transport.isConnected())
    {
        logger::debug("Dropping forwarded packet ssrc %u, seq %u, transport %s not connected",
            "VideoForwarderRewriteAndSendJob",
            rtpHeader->ssrc.get(),
            rtpHeader->sequenceNumber.get(),
            _transport.getLoggableId().c_str());

        return;
    }

    if (isRetransmittedPacket)
    {
        return;
    }

    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
