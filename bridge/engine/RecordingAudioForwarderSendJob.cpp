#include "bridge/engine/RecordingAudioForwarderSendJob.h"
#include "bridge/engine/AudioRewriter.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/RecordingTransport.h"

namespace bridge
{
RecordingAudioForwarderSendJob::RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
    memory::UniquePacket packet,
    transport::RecordingTransport& transport,
    const uint32_t extendedSequenceNumber)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _packet(std::move(packet)),
      _transport(transport),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void RecordingAudioForwarderSendJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader || !_transport.isConnected())
    {
        if (!_transport.isConnected())
        {
            logger::debug("Dropping forwarded packet ssrc %u, seq %u. Not connected",
                "RecordingAudioForwarderSendJob",
                uint32_t(rtpHeader->ssrc),
                uint16_t(rtpHeader->sequenceNumber));
        }

        return;
    }

    uint16_t nextSequenceNumber = 0;
    if (!_outboundContext.rewrite.shouldSend(rtpHeader->ssrc, _extendedSequenceNumber))
    {
        logger::debug("Dropping rec audio packet - sequence number...", "RecordingAudioForwarderSendJob");
        return;
    }

    bridge::AudioRewriter::rewrite(_outboundContext, _extendedSequenceNumber, *rtpHeader);

    if (_outboundContext.packetCache.isSet())
    {
        auto packetCache = _outboundContext.packetCache.get();
        if (packetCache)
        {
            if (!packetCache->add(*_packet, nextSequenceNumber))
            {
                logger::info("Dropping rec audio packet - cache issues...", "RecordingAudioForwarderSendJob");
                return;
            }
        }
    }

    _transport.protectAndSend(std::move(_packet));
}
} // namespace bridge
