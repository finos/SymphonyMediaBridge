#include "bridge/engine/ProcessUnackedRecordingEventPacketsJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/RecordingOutboundContext.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "logger/Logger.h"
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
    uint64_t timestamp = utils::Time::getAbsoluteTime();
    if (!_unackedPacketsTracker.shouldProcess(timestamp))
    {
        return;
    }

    std::array<uint16_t, UnackedPacketsTracker::maxUnackedPackets> unackedSequenceNumbers{};
    const auto numMissingSequenceNumbers = _unackedPacketsTracker.process(timestamp, unackedSequenceNumbers);

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
    auto cachedPacket = _recordingOutboundContext.packetCache.get(sequenceNumber);
    if (!cachedPacket)
    {
        return;
    }

    if (!recp::isRecPacket(cachedPacket->get(), cachedPacket->getLength()))
    {
        return;
    }

    auto packet = memory::makeUniquePacket(_allocator, *cachedPacket);
    if (!packet)
    {
        return;
    }

#if DEBUG_REC_EVENT_ACK
    logger::debug("Sending cached rec event packet seq %u", "ProcessUnackedRecordingEventPacketsJob", sequenceNumber);
#endif

    _transport.protectAndSend(std::move(packet));
}
} // namespace bridge
