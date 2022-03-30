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
    RecordingSendEventJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RecordingTransport& transport,
        PacketCache& recEventPacketCache,
        UnackedPacketsTracker& unackedPacketsTracker);

    ~RecordingSendEventJob() override;

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    transport::RecordingTransport& _transport;
    PacketCache& _recEventPacketCache;
    UnackedPacketsTracker& _unackedPacketsTracker;
};

} // namespace bridge
