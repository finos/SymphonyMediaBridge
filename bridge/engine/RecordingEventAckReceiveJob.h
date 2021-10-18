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

class RecordingEventAckReceiveJob : public jobmanager::CountedJob
{
public:
    RecordingEventAckReceiveJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RecordingTransport* sender,
        UnackedPacketsTracker& recEventUnackedPacketsTracker);

    ~RecordingEventAckReceiveJob() override;

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    UnackedPacketsTracker& _recEventUnackedPacketsTracker;
};

} // namespace bridge
