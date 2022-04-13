#pragma once

#include "transport/recp/RecHeader.h"

namespace recp
{

struct RecStreamRemovedEvent
{
    constexpr static size_t MIN_SIZE = REC_HEADER_SIZE + 4;

    RecHeader header;
    nwuint32_t ssrc;

    static const RecStreamRemovedEvent* fromPtr(const void* pointer, size_t len)
    {
        assert((intptr_t)pointer % alignof(RecStreamRemovedEvent) == 0);
        assert(len >= MIN_SIZE);
        return reinterpret_cast<const RecStreamRemovedEvent*>(pointer);
    }

    static RecStreamRemovedEvent* fromPtr(void* pointer, size_t len)
    {
        return const_cast<RecStreamRemovedEvent*>(fromPtr(static_cast<const void*>(pointer), len));
    }

    static RecStreamRemovedEvent* fromPacket(memory::Packet& packet)
    {
        return fromPtr(packet.get(), packet.getLength());
    }

    static const RecStreamRemovedEvent* fromPacket(const memory::Packet& packet)
    {
        return fromPtr(packet.get(), packet.getLength());
    }
};

// Ensure no padding will be added
static_assert(sizeof(RecStreamRemovedEvent) == RecStreamRemovedEvent::MIN_SIZE,
    "sizeof(RecStreamRemovedEvent) != RecStreamRemovedEvent::MIN_SIZE");

} // namespace recp
