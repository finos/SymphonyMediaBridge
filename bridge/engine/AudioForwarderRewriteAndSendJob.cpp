#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"
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

    for (auto& rtpHeaderExtension : headerExtensions->extensions())
    {
        if (senderInboundContext._rtpMap._audioLevelExtId.isSet() &&
            rtpHeaderExtension.getId() == senderInboundContext._rtpMap._audioLevelExtId.get())
        {
            rtpHeaderExtension.setId(receiverOutboundContext._rtpMap._audioLevelExtId.get());
        }
        else if (senderInboundContext._rtpMap._absSendTimeExtId.isSet() &&
            rtpHeaderExtension.getId() == senderInboundContext._rtpMap._absSendTimeExtId.get())
        {
            rtpHeaderExtension.setId(receiverOutboundContext._rtpMap._absSendTimeExtId.get());
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

    bool newSource = _outboundContext._lastRewrittenSsrc != header->ssrc;
    _outboundContext._lastRewrittenSsrc = header->ssrc;
    if (newSource)
    {
        _outboundContext._timestampOffset = _outboundContext._lastSentTimestamp +
            codec::Opus::sampleRate / codec::Opus::packetsPerSecond - header->timestamp.get();
        _outboundContext._highestSeenExtendedSequenceNumber = _extendedSequenceNumber - 1;
    }

    uint16_t nextSequenceNumber;
    if (!utils::OutboundSequenceNumber::process(_extendedSequenceNumber,
            _outboundContext._highestSeenExtendedSequenceNumber,
            _outboundContext._sequenceCounter,
            nextSequenceNumber))
    {
        logger::warn("%s dropping packet ssrc %u, seq %u, timestamp %u",
            "AudioForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            header->ssrc.get(),
            header->sequenceNumber.get(),
            header->timestamp.get());
        return;
    }

    header->ssrc = _outboundContext._ssrc;
    header->sequenceNumber = nextSequenceNumber;
    header->timestamp = _outboundContext._timestampOffset + header->timestamp;
    header->payloadType = _outboundContext._rtpMap._payloadType;
    if (static_cast<int32_t>(header->timestamp - _outboundContext._lastSentTimestamp) > 0)
    {
        _outboundContext._lastSentTimestamp = header->timestamp;
    }

    rewriteHeaderExtensions(header, _senderInboundContext, _outboundContext);
    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
