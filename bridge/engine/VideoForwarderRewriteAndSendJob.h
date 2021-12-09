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
class Transport;
} // namespace transport

namespace bridge
{

class SsrcOutboundContext;
class SsrcInboundContext;

class VideoForwarderRewriteAndSendJob : public jobmanager::CountedJob
{
public:
    VideoForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
        SsrcInboundContext& senderInboundContext,
        memory::Packet* packet,
        transport::Transport& transport,
        const uint32_t extendedSequenceNumber);

    virtual ~VideoForwarderRewriteAndSendJob();
    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    SsrcInboundContext& _senderInboundContext;
    memory::Packet* _packet;
    transport::Transport& _transport;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
