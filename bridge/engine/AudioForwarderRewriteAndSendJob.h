#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
namespace transport
{
class Transport;

} // namespace transport

namespace bridge
{
class SsrcOutboundContext;

class AudioForwarderRewriteAndSendJob : public jobmanager::CountedJob
{
public:
    AudioForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
        memory::PacketPtr packet,
        const uint32_t extendedSequenceNumber,
        transport::Transport& transport);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::PacketPtr _packet;
    uint32_t _extendedSequenceNumber;
    transport::Transport& _transport;
};

} // namespace bridge
