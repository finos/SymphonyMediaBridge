#pragma once

#include "bridge/engine/RtpForwarderReceiveBaseJob.h"
#include "memory/PacketPoolAllocator.h"

namespace memory
{
class Packet;
}

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class VideoForwarderReceiveJob : public RtpForwarderReceiveBaseJob
{
public:
    VideoForwarderReceiveJob(memory::UniquePacket packet,
        memory::PacketPoolAllocator& allocator,
        transport::RtcTransport* sender,
        bridge::EngineMixer& engineMixer,
        bridge::SsrcInboundContext& ssrcContext,
        const uint32_t localVideoSsrc,
        const uint32_t extendedSequenceNumber,
        const uint64_t timestamp);

    void run() override;

private:
    memory::PacketPoolAllocator& _allocator;
    uint32_t _localVideoSsrc;
    uint64_t _timestamp;
};

} // namespace bridge
