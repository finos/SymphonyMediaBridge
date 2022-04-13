#include "bridge/engine/RecordingRtpNackReceiveJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "rtp/RtpHeader.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecControlHeader.h"

#define DEBUG_REC_NACK_EVENT 0

namespace bridge
{
RecordingRtpNackReceiveJob::RecordingRtpNackReceiveJob(memory::UniquePacket packet,
    memory::PacketPoolAllocator& allocator,
    transport::RecordingTransport* sender,
    SsrcOutboundContext& ssrcOutboundContext)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _allocator(allocator),
      _sender(sender),
      _ssrcOutboundContext(ssrcOutboundContext)
{
}

void RecordingRtpNackReceiveJob::run()
{
    auto recControlHeader = recp::RecControlHeader::fromPacket(*_packet);
    if (!recControlHeader)
    {
        return;
    }

    if (!_ssrcOutboundContext._packetCache.isSet() || !_ssrcOutboundContext._packetCache.get())
    {
        return;
    }

    auto cachedPacket = _ssrcOutboundContext._packetCache.get()->get(recControlHeader->sequenceNumber);
    if (!cachedPacket)
    {
        return;
    }

    if (!rtp::isRtpPacket(cachedPacket->get(), cachedPacket->getLength()))
    {
        return;
    }

    auto packet = memory::makeUniquePacket(_allocator, *cachedPacket);
    if (!packet)
    {
        return;
    }

#if DEBUG_REC_NACK_EVENT
    logger::debug("Sending NACKed packet for ssrc %u sequence %u",
        "RecordingRtpNackReceiveJob",
        _ssrcOutboundContext._ssrc,
        recControlHeader->sequenceNumber.get());
#endif

    _sender->protectAndSend(std::move(packet));
}

} // namespace bridge
