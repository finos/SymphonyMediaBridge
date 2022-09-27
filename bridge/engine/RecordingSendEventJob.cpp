#include "bridge/engine/RecordingSendEventJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecHeader.h"

namespace bridge
{

RecordingSendEventJob::RecordingSendEventJob(memory::UniquePacket packet,
    transport::RecordingTransport& transport,
    PacketCache& recEventPacketCache,
    UnackedPacketsTracker& unackedPacketsTracker)
    : CountedJob(transport.getJobCounter()),
      _packet(std::move(packet)),
      _transport(transport),
      _recEventPacketCache(recEventPacketCache),
      _unackedPacketsTracker(unackedPacketsTracker)
{
}

void RecordingSendEventJob::run()
{
    auto recHeader = recp::RecHeader::fromPacket(*_packet);
    if (!recHeader)
    {
        return;
    }

    const auto sequenceNumber = recHeader->sequenceNumber.get();
    _recEventPacketCache.add(*_packet, sequenceNumber);
    _transport.protectAndSend(std::move(_packet));
    _unackedPacketsTracker.onPacketSent(sequenceNumber, utils::Time::getAbsoluteTime());
}

} // namespace bridge
