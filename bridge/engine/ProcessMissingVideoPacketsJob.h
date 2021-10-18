#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
}

namespace bridge
{

class SsrcInboundContext;

class ProcessMissingVideoPacketsJob : public jobmanager::CountedJob
{
public:
    ProcessMissingVideoPacketsJob(SsrcInboundContext& ssrcContext,
        const uint32_t reporterSsrc,
        transport::RtcTransport& transport,
        memory::PacketPoolAllocator& allocator);

    void run() override;

private:
    SsrcInboundContext& _ssrcContext;
    uint32_t _reporterSsrc;
    transport::RtcTransport& _transport;
    memory::PacketPoolAllocator& _allocator;
};

} // namespace bridge
