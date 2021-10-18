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

class AudioForwarderRewriteAndSendJob : public jobmanager::CountedJob
{
public:
    AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
        memory::Packet* packet,
        const uint32_t extendedSequenceNumber,
        transport::Transport& transport);

    virtual ~AudioForwarderRewriteAndSendJob();

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::Packet* _packet;
    uint32_t _extendedSequenceNumber;
    transport::Transport& _transport;
};

} // namespace bridge
