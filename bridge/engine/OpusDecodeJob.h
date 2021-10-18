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
class DataReceiver;
} // namespace transport

namespace bridge
{
class EngineMixer;
class SsrcInboundContext;
} // namespace bridge

namespace codec
{
class OpusDecoder;
}

namespace bridge
{

class OpusDecodeJob : public jobmanager::CountedJob
{
public:
    OpusDecodeJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RtcTransport* sender,
        bridge::EngineMixer& engineMixer,
        bridge::SsrcInboundContext& ssrcContext);

    virtual ~OpusDecodeJob();

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    bridge::EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    bridge::SsrcInboundContext& _ssrcContext;

    void onPacketDecoded(const int32_t decodedFrames,
        const uint8_t* decodedData,
        uint8_t* payloadStart,
        const size_t headerLength);
};

} // namespace bridge
