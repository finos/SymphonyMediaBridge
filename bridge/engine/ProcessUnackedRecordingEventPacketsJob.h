#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

class UnackedPacketsTracker;
class RecordingOutboundContext;

class ProcessUnackedRecordingEventPacketsJob : public jobmanager::CountedJob
{
public:
    ProcessUnackedRecordingEventPacketsJob(RecordingOutboundContext& recordingOutboundContext,
        UnackedPacketsTracker& unackedPacketsTracker,
        transport::RecordingTransport& transport,
        memory::PacketPoolAllocator& allocator);

    void run() override;

private:
    RecordingOutboundContext& _recordingOutboundContext;
    UnackedPacketsTracker& _unackedPacketsTracker;
    transport::RecordingTransport& _transport;
    memory::PacketPoolAllocator& _allocator;

    void sendIfCached(const uint16_t sequenceNumber);
};

} // namespace bridge
