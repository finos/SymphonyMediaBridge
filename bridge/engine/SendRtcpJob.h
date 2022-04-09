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
    SendRtcpJob(memory::UniquePacket rtcpPacket, transport::Transport& transport);

    void run() override;

private:
    transport::Transport& _transport;
    memory::UniquePacket _packet;
};

} // namespace bridge
