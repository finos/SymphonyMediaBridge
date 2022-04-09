#include "test/transport/SrtpProtectJob.h"
#include "transport/Transport.h"

namespace transport
{

SrtpProtectJob::SrtpProtectJob(std::atomic_uint32_t& ownerJobsCounter,
    memory::UniquePacket packet,
    transport::Transport& transport)
    : jobmanager::CountedJob(ownerJobsCounter),
      _packet(std::move(packet)),
      _transport(transport)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void SrtpProtectJob::run()
{
    _transport.protectAndSend(std::move(_packet));
}

} // namespace transport
