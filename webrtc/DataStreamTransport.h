#pragma once

#include "memory/PoolBuffer.h"
#include "memory/PacketPoolAllocator.h"

namespace webrtc
{
class DataStreamTransport
{
public:
    // Expects packet with SctpStreamMessageHeader
    virtual bool sendSctp(uint16_t streamId,
        uint32_t protocolId,
        memory::PoolBuffer<memory::PacketPoolAllocator> buffer) = 0;
    virtual uint16_t allocateOutboundSctpStream() = 0;
    virtual memory::PacketPoolAllocator& getAllocator() = 0;
};
} // namespace webrtc
