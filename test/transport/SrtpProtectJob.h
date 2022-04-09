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
    SrtpProtectJob(std::atomic_uint32_t& ownerJobsCounter, memory::UniquePacket packet, transport::Transport& transport);

    void run() override;

private:
    memory::UniquePacket _packet;
    transport::Transport& _transport;
};

} // namespace transport
