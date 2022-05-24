#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

namespace transport
{
class Transport;
} // namespace transport

namespace bridge
{

class SsrcInboundContext;
class SsrcOutboundContext;

class AudioForwarderRewriteAndSendJob : public jobmanager::CountedJob
{
public:
    AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
        SsrcInboundContext& senderInboundContext,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber,
        transport::Transport& transport);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    SsrcInboundContext& _senderInboundContext;
    memory::UniquePacket _packet;
    uint32_t _extendedSequenceNumber;
    transport::Transport& _transport;
};

} // namespace bridge
