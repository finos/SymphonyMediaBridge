#include "bridge/engine/RecordingEventAckReceiveJob.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecControlHeader.h"

#define DEBUG_REC_ACK_EVENT 0

namespace bridge
{

RecordingEventAckReceiveJob::RecordingEventAckReceiveJob(memory::PacketPtr packet,
    transport::RecordingTransport* sender,
    UnackedPacketsTracker& recEventUnackedPacketsTracker)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _recEventUnackedPacketsTracker(recEventUnackedPacketsTracker)
{
}

void RecordingEventAckReceiveJob::run()
{
    if (!recp::isRecControlPacket(_packet->get(), _packet->getLength()))
    {
        return;
    }

    auto recControlHeader = recp::RecControlHeader::fromPacket(*_packet);
    if (!recControlHeader->isEventAck())
    {
        return;
    }

    auto sequenceNumber = recControlHeader->sequenceNumber;
    uint32_t extendedSequenceNumber;
    _recEventUnackedPacketsTracker.onPacketAcked(sequenceNumber, extendedSequenceNumber);

#if DEBUG_REC_ACK_EVENT
    logger::debug("Received recording event ack for seq %hu %u",
        "RecordingEventAckReceiveJob",
        sequenceNumber.get(),
        extendedSequenceNumber);
#endif
}

} // namespace bridge
