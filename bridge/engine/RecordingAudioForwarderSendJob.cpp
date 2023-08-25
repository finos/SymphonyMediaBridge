#include "bridge/engine/RecordingAudioForwarderSendJob.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/AudioRewriter.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/RecordingTransport.h"
#include "utils/Function.h"

namespace bridge
{
RecordingAudioForwarderSendJob::RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
    memory::UniquePacket packet,
    transport::RecordingTransport& transport,
    const uint32_t extendedSequenceNumber,
    MixerManagerAsync& mixerManager,
    size_t endpointIdHash,
    EngineMixer& mixer,
    uint64_t timestamp)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
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

    uint16_t nextSequenceNumber = 0; // TODO never set
    if (!_outboundContext.shouldSend(rtpHeader->ssrc, _extendedSequenceNumber))
    {
        logger::debug("Dropping rec audio packet - sequence number...", "RecordingAudioForwarderSendJob");
        return;
    }

    bridge::AudioRewriter::rewrite(_outboundContext, _extendedSequenceNumber, *rtpHeader, _timestamp);

    if (_outboundContext.packetCache.isSet())
    {
        auto packetCache = _outboundContext.packetCache.get();
        if (packetCache)
        {
            if (!packetCache->add(*_packet, nextSequenceNumber))
            {
                logger::warn("Failed to cache rec audio packet", "RecordingAudioForwarderSendJob");
            }
        }
    }
    else
    {
        logger::debug("New ssrc %u seen on %s, sending request to add AudioPacketCache to transport",
            "RecordingAudioForwarderSendJob",
            _outboundContext.ssrc,
            _transport.getLoggableId().c_str());

        _outboundContext.packetCache.set(nullptr);
        _mixerManager.asyncAllocateRecordingRtpPacketCache(_mixer, _outboundContext.ssrc, _endpointIdHash);
    }

    _transport.protectAndSend(std::move(_packet));
}
} // namespace bridge
