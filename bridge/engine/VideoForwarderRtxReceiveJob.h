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
    VideoForwarderRtxReceiveJob(memory::PacketPtr packet,
        transport::RtcTransport* sender,
        bridge::EngineMixer& engineMixer,
        bridge::SsrcInboundContext& ssrcContext,
        const uint32_t mainSsrc,
        const uint32_t extendedSequenceNumber);

    void run() override;

private:
    memory::PacketPtr _packet;
    bridge::EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    bridge::SsrcInboundContext& _ssrcContext;
    uint32_t _mainSsrc;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
