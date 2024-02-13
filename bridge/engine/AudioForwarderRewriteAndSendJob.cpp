#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"

namespace bridge
{

AudioForwarderRewriteAndSendJob::AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    SsrcInboundContext& senderInboundContext,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    transport::Transport& transport,
    uint64_t timestamp)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _senderInboundContext(senderInboundContext),
      _packet(std::move(packet)),
      _extendedSequenceNumber(extendedSequenceNumber),
      _transport(transport),
      _timestamp(timestamp)
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

    const bool isTelephoneEvent = !_senderInboundContext.telephoneEventRtpMap.isEmpty() &&
        header->payloadType == _senderInboundContext.telephoneEventRtpMap.payloadType;

    if (isTelephoneEvent && _outboundContext.telephoneEventRtpMap.isEmpty())
    {
        _outboundContext.dropTelephoneEvent(_extendedSequenceNumber, header->ssrc);
        return;
    }

    if (!_outboundContext
             .rewriteAudio(*header, _senderInboundContext, _extendedSequenceNumber, _timestamp, isTelephoneEvent))
    {
        logger::warn("%s dropping packet ssrc %u, seq %u, timestamp %u, last sent seq %u, offset %d",
            "AudioForwarderRewriteAndSendJob",
            _transport.getLoggableId().c_str(),
            header->ssrc.get(),
            _extendedSequenceNumber,
            header->timestamp.get(),
            _outboundContext.getLastSentSequenceNumber(),
            _outboundContext.getSequenceNumberOffset());
        return;
    }

    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
