#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"
#include "utils/OutboundSequenceNumber.h"

namespace bridge
{

AudioForwarderRewriteAndSendJob::AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
    memory::PacketPtr packet,
    const uint32_t extendedSequenceNumber,
    transport::Transport& transport)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
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
        return;
    }

    header->ssrc = _outboundContext._ssrc;
    header->sequenceNumber = nextSequenceNumber;
    header->timestamp = _outboundContext._timestampOffset + header->timestamp;
    if (static_cast<int32_t>(header->timestamp - _outboundContext._lastSentTimestamp) > 0)
    {
        _outboundContext._lastSentTimestamp = header->timestamp;
    }

    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
