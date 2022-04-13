#pragma once

#include "transport/recp/RecHeader.h"

namespace recp
{

struct RecStreamAddedEvent
{
    constexpr static size_t MIN_SIZE = REC_HEADER_SIZE + 8;

    RecHeader header;
    nwuint32_t ssrc;
    uint8_t rtpPayload : 7;
    uint8_t isScreenSharing : 1;
    uint8_t codec;
    nwuint16_t endpointLen;

    static const RecStreamAddedEvent* fromPtr(const void* pointer, size_t len)
    {
        assert((intptr_t)pointer % alignof(RecStreamAddedEvent) == 0);
        assert(len >= MIN_SIZE);
        return reinterpret_cast<const RecStreamAddedEvent*>(pointer);
    }

    static const RecStreamAddedEvent* fromPacket(const memory::Packet& packet)
    {
        return fromPtr(packet.get(), packet.getLength());
    }
};

// Ensure no padding will be added
static_assert(sizeof(RecStreamAddedEvent) == RecStreamAddedEvent::MIN_SIZE,
    "sizeof(RecStreamAddedEvent) != RecStreamAddedEvent::MIN_SIZE");

} // namespace recp
