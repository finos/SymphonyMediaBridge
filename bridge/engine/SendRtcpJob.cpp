#include "bridge/engine/SendRtcpJob.h"
#include "transport/Transport.h"

namespace bridge
{

SendRtcpJob::SendRtcpJob(memory::UniquePacket rtcpPacket, transport::Transport& transport)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _transport(transport),
      _packet(std::move(rtcpPacket))
{
}

void SendRtcpJob::run()
{
    _transport.protectAndSend(std::move(_packet));
}

} // namespace bridge
