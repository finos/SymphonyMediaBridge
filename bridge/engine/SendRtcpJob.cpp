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

SendRtcpJob::~SendRtcpJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
    }
}

void SendRtcpJob::run()
{
    _transport.protectAndSend(_packet, _allocator);
    _packet = nullptr;
}

} // namespace bridge
