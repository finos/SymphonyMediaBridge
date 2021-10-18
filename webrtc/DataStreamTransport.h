#pragma once

#include "memory/PacketPoolAllocator.h"

namespace webrtc
{
class DataStreamTransport
{
public:
    // Expects packet with SctpStreamMessageHeader
    virtual bool sendSctp(uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length) = 0;
    virtual uint16_t allocateOutboundSctpStream() = 0;
};
} // namespace webrtc
