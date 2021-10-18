#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>

namespace memory
{
class Packet;
}

namespace transport
{
class SrtpClient;
class Transport;
} // namespace transport

namespace bridge
{

class SsrcOutboundContext;

class ForwarderSendJob : public jobmanager::CountedJob
{
public:
    ForwarderSendJob(SsrcOutboundContext& outboundContext,
        memory::Packet* packet,
        const uint32_t extendedSequenceNumber,
        transport::Transport& transport);

    virtual ~ForwarderSendJob();

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::Packet* _packet;
    uint32_t _extendedSequenceNumber;
    transport::Transport& _transport;
};

} // namespace bridge
