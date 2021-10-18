#include "bridge/engine/ForwarderSendJob.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"
#include "utils/OutboundSequenceNumber.h"

namespace bridge
{

ForwarderSendJob::ForwarderSendJob(SsrcOutboundContext& outboundContext,
    memory::Packet* packet,
    const uint32_t extendedSequenceNumber,
    transport::Transport& transport)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _packet(packet),
      _extendedSequenceNumber(extendedSequenceNumber),
      _transport(transport)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

ForwarderSendJob::~ForwarderSendJob()
{
    if (_packet)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
    }
}

void ForwarderSendJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader || !_transport.isConnected())
    {
        if (!_transport.isConnected())
        {
            logger::debug("Dropping forwarded packet ssrc %u, seq %u. Not connected",
                "ForwarderSendJob",
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
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    rtpHeader->sequenceNumber = nextSequenceNumber;
    _transport.protectAndSend(_packet, _outboundContext._allocator);
    _packet = nullptr;
}

} // namespace bridge
