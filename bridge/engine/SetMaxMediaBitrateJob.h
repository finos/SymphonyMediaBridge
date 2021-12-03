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
class SetMaxMediaBitrateJob : public jobmanager::CountedJob
{
public:
    SetMaxMediaBitrateJob(transport::RtcTransport& transport,
        uint32_t reporterSsrc,
        uint32_t ssrc,
        uint32_t bitrateKbps,
        memory::PacketPoolAllocator& allocator);

    void run() override;

private:
    transport::RtcTransport& _transport;
    const uint32_t _ssrc;
    const uint32_t _reporterSsrc;
    const uint32_t _bitrate;
    memory::PacketPoolAllocator& _allocator;
};
} // namespace bridge
