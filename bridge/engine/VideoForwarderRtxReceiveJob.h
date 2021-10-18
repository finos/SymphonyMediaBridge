#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

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

class VideoForwarderRtxReceiveJob : public jobmanager::CountedJob
{
public:
    VideoForwarderRtxReceiveJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RtcTransport* sender,
        bridge::EngineMixer& engineMixer,
        bridge::SsrcInboundContext& ssrcContext,
        const uint32_t mainSsrc,
        const uint32_t extendedSequenceNumber);

    virtual ~VideoForwarderRtxReceiveJob();
    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    bridge::EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    bridge::SsrcInboundContext& _ssrcContext;
    uint32_t _mainSsrc;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
