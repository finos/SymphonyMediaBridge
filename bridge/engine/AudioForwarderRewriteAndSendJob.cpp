#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/AudioRewriter.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"

namespace
{

inline void rewriteHeaderExtensions(rtp::RtpHeader& rtpHeader,
    const bridge::SsrcInboundContext& senderInboundContext,
    const bridge::SsrcOutboundContext& receiverOutboundContext)
{
    const auto headerExtensions = rtpHeader.getExtensionHeader();
    if (!headerExtensions)
    {
        return;
    }

    for (auto& rtpHeaderExtension : headerExtensions->extensions())
    {
        if (senderInboundContext.rtpMap.audioLevelExtId.isSet() &&
            rtpHeaderExtension.getId() == senderInboundContext.rtpMap.audioLevelExtId.get())
        {
            rtpHeaderExtension.setId(receiverOutboundContext.rtpMap.audioLevelExtId.get());
        }
        else if (senderInboundContext.rtpMap.absSendTimeExtId.isSet() &&
            rtpHeaderExtension.getId() == senderInboundContext.rtpMap.absSendTimeExtId.get())
        {
            rtpHeaderExtension.setId(receiverOutboundContext.rtpMap.absSendTimeExtId.get());
        }
    }
}

} // namespace

namespace bridge
{

AudioForwarderRewriteAndSendJob::AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    SsrcInboundContext& senderInboundContext,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    transport::Transport& transport)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _senderInboundContext(senderInboundContext),
      _packet(std::move(packet)),
      _extendedSequenceNumber(extendedSequenceNumber),
      _transport(transport)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void AudioForwarderRewriteAndSendJob::run()
{
    auto header = rtp::RtpHeader::fromPacket(*_packet);
    if (!header)
    {
        return;
    }

    if (!_outboundContext.shouldSend(header->ssrc.get(), _extendedSequenceNumber))
    {
        logger::warn("%s dropping packet ssrc %u, seq %u, timestamp %u, last sent seq %u, offset %d",
            "AudioForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            header->ssrc.get(),
            _extendedSequenceNumber,
            header->timestamp.get(),
            _outboundContext.rewrite.lastSent.sequenceNumber,
            _outboundContext.rewrite.offset.sequenceNumber);
        return;
    }

    bridge::AudioRewriter::rewrite(_outboundContext, _extendedSequenceNumber, *header);

    rewriteHeaderExtensions(*header, _senderInboundContext, _outboundContext);
    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
