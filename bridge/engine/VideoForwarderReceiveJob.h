#pragma once

#include "jobmanager/Job.h"
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

class EngineMixer;
class SsrcInboundContext;

class VideoForwarderReceiveJob : public jobmanager::CountedJob
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
    memory::UniquePacket _packet;
    memory::PacketPoolAllocator& _allocator;
    bridge::EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    bridge::SsrcInboundContext& _ssrcContext;
    uint32_t _localVideoSsrc;
    uint32_t _extendedSequenceNumber;
    uint64_t _timestamp;
};

} // namespace bridge
