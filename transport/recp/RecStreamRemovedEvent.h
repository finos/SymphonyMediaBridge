#pragma once

#include "transport/recp/RecHeader.h"

namespace recp
{

struct RecStreamRemovedEvent
{
    constexpr static size_t MIN_SIZE = REC_HEADER_SIZE + 4;

    RecHeader header;
    nwuint32_t ssrc;

    static RecStreamRemovedEvent* fromPtr(void* pointer, size_t len)
    {
        assert((intptr_t)pointer % alignof(RecStreamRemovedEvent) == 0);
        assert(len >= MIN_SIZE);
        return reinterpret_cast<RecStreamRemovedEvent*>(pointer);
    }

    static RecStreamRemovedEvent* fromPacket(memory::Packet& packet)
    {
        return fromPtr(packet.get(), packet.getLength());
    }
};

// Ensure no padding will be added
static_assert(sizeof(RecStreamRemovedEvent) == RecStreamRemovedEvent::MIN_SIZE,
    "sizeof(RecStreamRemovedEvent) != RecStreamRemovedEvent::MIN_SIZE");

} // namespace recp
