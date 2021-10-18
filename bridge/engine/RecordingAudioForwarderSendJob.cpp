#include "bridge/engine/RecordingAudioForwarderSendJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/RecordingTransport.h"
#include "utils/OutboundSequenceNumber.h"

namespace bridge
{
RecordingAudioForwarderSendJob::RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
    memory::Packet* packet,
    transport::RecordingTransport& transport,
    const uint32_t extendedSequenceNumber)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _packet(packet),
      _transport(transport),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

RecordingAudioForwarderSendJob::~RecordingAudioForwarderSendJob()
{
    if (_packet)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
    }
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

        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    uint16_t nextSequenceNumber;
    if (!utils::OutboundSequenceNumber::process(_extendedSequenceNumber,
            _outboundContext._highestSeenExtendedSequenceNumber,
            _outboundContext._sequenceCounter,
            nextSequenceNumber))
    {
        logger::debug("Dropping rec audio packet - sequence number...", "RecordingAudioForwarderSendJob");
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    rtpHeader->sequenceNumber = nextSequenceNumber;
    if (_outboundContext._packetCache.isSet())
    {
        auto packetCache = _outboundContext._packetCache.get();
        if (packetCache)
        {
            if (!packetCache->add(_packet, nextSequenceNumber))
            {
                logger::info("Dropping rec audio packet - cache issues...", "RecordingAudioForwarderSendJob");
                _outboundContext._allocator.free(_packet);
                _packet = nullptr;
                return;
            }
        }
    }

    _transport.protectAndSend(_packet, _outboundContext._allocator);
    _packet = nullptr;
}
} // namespace bridge
