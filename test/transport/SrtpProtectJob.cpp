#include "test/transport/SrtpProtectJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "jobmanager/JobManager.h"
#include "transport/Transport.h"
#include "transport/dtls/SrtpClient.h"

namespace transport
{

SrtpProtectJob::SrtpProtectJob(std::atomic_uint32_t& ownerJobsCounter,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    transport::Transport& transport)
    : jobmanager::CountedJob(ownerJobsCounter),
      _packet(packet),
      _allocator(allocator),
      _transport(transport)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

void SrtpProtectJob::run()
{
    _transport.protectAndSend(_packet, _allocator);
}

} // namespace transport
