#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace memory
{
class Packet;
}

namespace transport
{

class Transport;

class SrtpProtectJob : public jobmanager::CountedJob
{
public:
    SrtpProtectJob(std::atomic_uint32_t& ownerJobsCounter,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::Transport& transport);

    ~SrtpProtectJob();

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    transport::Transport& _transport;
};

} // namespace transport
