#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class Transport;
} // namespace transport

namespace bridge
{

class SendRtcpJob : public jobmanager::CountedJob
{
public:
    SendRtcpJob(memory::Packet* rtcpPacket, transport::Transport& transport, memory::PacketPoolAllocator& allocator);

    void run() override;

private:
    transport::Transport& _transport;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
};

} // namespace bridge
