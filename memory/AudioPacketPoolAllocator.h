#pragma once

#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PoolAllocator.h"

namespace memory
{

class AudioPacket : public FixedPacket<5800>
{
};

using AudioPacketPoolAllocator = PoolAllocator<sizeof(AudioPacket)>;

inline AudioPacket* makePacket(AudioPacketPoolAllocator& allocator)
{
    auto pointer = allocator.allocate();
    assert(pointer);
    if (!pointer)
    {
        logger::error("Unable to allocate packet, no space left in pool %s",
            "AudioPacketPoolAllocator",
            allocator.getName().c_str());
        return nullptr;
    }

    return new (pointer) memory::AudioPacket();
}

inline AudioPacket* makePacket(AudioPacketPoolAllocator& allocator, const void* data, size_t length)
{
    assert(length <= memory::AudioPacket::size);
    if (length > memory::AudioPacket::size)
    {
        return nullptr;
    }

    auto packet = makePacket(allocator);
    if (!packet)
    {
        return packet;
    }

    std::memcpy(packet->get(), data, length);
    packet->setLength(length);
    return packet;
}

inline AudioPacket* makePacket(AudioPacketPoolAllocator& allocator, const Packet& packet)
{
    return makePacket(allocator, packet.get(), packet.getLength());
}

} // namespace memory
