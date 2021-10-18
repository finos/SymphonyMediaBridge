#pragma once

#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>

namespace bridge
{
class SsrcOutboundContext;
class EngineMixer;

} // namespace bridge
namespace memory
{
class Packet;
}

namespace jobmanager
{
class JobManager;
}

namespace rtp
{
struct RtpHeader;
}

namespace transport
{

class SrtpClient;

class Transport;

class SrtpProtectJob : public jobmanager::CountedJob
{
public:
    SrtpProtectJob(std::atomic_uint32_t& ownerJobsCounter,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::Transport& transport);

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    transport::Transport& _transport;
};

} // namespace transport
