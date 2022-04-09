#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

class PacketCache;
class UnackedPacketsTracker;

class RecordingSendEventJob : public jobmanager::CountedJob
{
public:
    RecordingSendEventJob(memory::PacketPtr packet,
        transport::RecordingTransport& transport,
        PacketCache& recEventPacketCache,
        UnackedPacketsTracker& unackedPacketsTracker);

    void run() override;

private:
    memory::PacketPtr _packet;
    transport::RecordingTransport& _transport;
    PacketCache& _recEventPacketCache;
    UnackedPacketsTracker& _unackedPacketsTracker;
};

} // namespace bridge
