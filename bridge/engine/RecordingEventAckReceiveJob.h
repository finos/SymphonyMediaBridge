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
    RecordingEventAckReceiveJob(memory::UniquePacket packet,
        transport::RecordingTransport* sender,
        UnackedPacketsTracker& recEventUnackedPacketsTracker);

    void run() override;

private:
    memory::UniquePacket _packet;
    UnackedPacketsTracker& _recEventUnackedPacketsTracker;
};

} // namespace bridge
