#include "bridge/engine/ProcessUnackedRecordingEventPacketsJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/RecordingOutboundContext.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "logger/Logger.h"
#include "memory/RefCountedPacket.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecHeader.h"

#define DEBUG_REC_EVENT_ACK 0

namespace bridge
{

ProcessUnackedRecordingEventPacketsJob::ProcessUnackedRecordingEventPacketsJob(
    RecordingOutboundContext& recordingOutboundContext,
    UnackedPacketsTracker& unackedPacketsTracker,
    transport::RecordingTransport& transport,
    memory::PacketPoolAllocator& allocator)
    : CountedJob(transport.getJobCounter()),
      _recordingOutboundContext(recordingOutboundContext),
      _unackedPacketsTracker(unackedPacketsTracker),
      _transport(transport),
      _allocator(allocator)
{
}

void ProcessUnackedRecordingEventPacketsJob::run()
{
    std::array<uint16_t, UnackedPacketsTracker::maxUnackedPackets> unackedSequenceNumbers{};
    const auto numMissingSequenceNumbers =
        _unackedPacketsTracker.process(utils::Time::getAbsoluteTime() / 1000000ULL, unackedSequenceNumbers);

    if (numMissingSequenceNumbers == 0)
    {
        return;
    }

    for (size_t i = 0; i < numMissingSequenceNumbers; ++i)
    {
        logger::debug("Processing unacked recording events %lu",
            "ProcessUnackedRecordingEventPacketsJob",
            numMissingSequenceNumbers);
        sendIfCached(unackedSequenceNumbers[i]);
    }
}

void ProcessUnackedRecordingEventPacketsJob::sendIfCached(const uint16_t sequenceNumber)
{
    auto scopedRef = memory::RefCountedPacket::ScopedRef(_recordingOutboundContext._packetCache.get(sequenceNumber));
    if (!scopedRef._refCountedPacket)
    {
        return;
    }
    auto cachedPacket = scopedRef._refCountedPacket->get();
    if (!cachedPacket)
    {
        return;
    }

    if (!recp::isRecPacket(cachedPacket->get(), cachedPacket->getLength()))
    {
        return;
    }

    auto packet = memory::makePacket(_allocator);
    if (!packet)
    {
        return;
    }

    memcpy(packet->get(), cachedPacket->get(), cachedPacket->getLength());
    packet->setLength(cachedPacket->getLength());

#if DEBUG_REC_EVENT_ACK
    logger::debug("Sending cached rec event packet seq %u", "ProcessUnackedRecordingEventPacketsJob", sequenceNumber);
#endif

    _transport.protectAndSend(packet, _allocator);
}
} // namespace bridge
