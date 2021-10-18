#include "bridge/engine/SendRtcpJob.h"
#include "transport/Transport.h"

namespace bridge
{

SendRtcpJob::SendRtcpJob(memory::Packet* rtcpPacket,
    transport::Transport& transport,
    memory::PacketPoolAllocator& allocator)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _transport(transport),
      _packet(rtcpPacket),
      _allocator(allocator)
{
}

void SendRtcpJob::run()
{
    _transport.protectAndSend(_packet, _allocator);
}

} // namespace bridge
