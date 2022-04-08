#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "bridge/engine/Vp8Rewriter.h"
#include "transport/Transport.h"
#include "utils/OutboundSequenceNumber.h"

namespace bridge
{

VideoForwarderRewriteAndSendJob::VideoForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    SsrcInboundContext& senderInboundContext,
    memory::Packet* packet,
    transport::Transport& transport,
    const uint32_t extendedSequenceNumber)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _senderInboundContext(senderInboundContext),
      _packet(packet),
      _transport(transport),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

VideoForwarderRewriteAndSendJob::~VideoForwarderRewriteAndSendJob()
{
    if (_packet)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
    }
}

void VideoForwarderRewriteAndSendJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    bool isRetransmittedPacket = false;
    if (rtpHeader->payloadType == static_cast<uint16_t>(RtpMap::Format::VP8RTX))
    {
        rtpHeader->payloadType = static_cast<uint16_t>(RtpMap::Format::VP8);
        isRetransmittedPacket = true;
    }

    const bool isKeyFrame = codec::Vp8Header::isKeyFrame(rtpHeader->getPayload(),
        codec::Vp8Header::getPayloadDescriptorSize(rtpHeader->getPayload(),
            _packet->getLength() - rtpHeader->headerLength()));

    const auto ssrc = rtpHeader->ssrc.get();
    if (ssrc != _outboundContext._lastRewrittenSsrc)
    {
        if (isRetransmittedPacket)
        {
            _outboundContext._allocator.free(_packet);
            _packet = nullptr;
            return;
        }
        if (!isKeyFrame)
        {
            _outboundContext._needsKeyframe = true;
            _senderInboundContext._pliScheduler.triggerPli();
        }
    }

    if (_outboundContext._needsKeyframe)
    {
        if (!isKeyFrame)
        {
            _outboundContext._allocator.free(_packet);
            _packet = nullptr;
            return;
        }
        else
        {
            _outboundContext._needsKeyframe = false;
        }
    }

    uint32_t rewrittenExtendedSequenceNumber = 0;
    if (!Vp8Rewriter::rewrite(_outboundContext,
            _packet,
            _outboundContext._ssrc,
            _extendedSequenceNumber,
            _transport.getLoggableId().c_str(),
            rewrittenExtendedSequenceNumber))
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    uint16_t nextSequenceNumber;
    if (!utils::OutboundSequenceNumber::process(rewrittenExtendedSequenceNumber,
            _outboundContext._highestSeenExtendedSequenceNumber,
            _outboundContext._sequenceCounter,
            nextSequenceNumber))
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }
    rtpHeader->sequenceNumber = nextSequenceNumber;
    if (isKeyFrame)
    {
        _outboundContext._lastKeyFrameSequenceNumber = nextSequenceNumber;
    }

    if (_outboundContext._packetCache.isSet() && _outboundContext._packetCache.get())
    {
        if (!_outboundContext._packetCache.get()->add(*_packet, nextSequenceNumber))
        {
            _outboundContext._allocator.free(_packet);
            _packet = nullptr;
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

        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    if (isRetransmittedPacket)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    _transport.protectAndSend(_packet, _outboundContext._allocator);
    _packet = nullptr;
}

} // namespace bridge
