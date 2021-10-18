#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>
#include <memory>

namespace jobmanager
{
class JobManager;
}

namespace transport
{
class SrtpClient;
class Transport;
class DataReceiver;
} // namespace transport

namespace rtp
{
enum RtcpPacketType : uint8_t;
}

namespace bridge
{
class SsrcOutboundContext;

class SendPliJob : public jobmanager::CountedJob
{
public:
    SendPliJob(const uint32_t fromSsrc,
        const uint32_t aboutSsrc,
        transport::Transport& transport,
        memory::PacketPoolAllocator& allocator);

    void run() override;

private:
    uint32_t _aboutSsrc;
    uint32_t _fromSsrc;
    transport::Transport& _transport;
    memory::PacketPoolAllocator& _allocator;
};

} // namespace bridge