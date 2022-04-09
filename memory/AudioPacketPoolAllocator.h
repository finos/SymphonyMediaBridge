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

// Be very careful with reset as the deleter is not changed if already set. You may try to deallocate
// packet in the wrong pool.
typedef std::unique_ptr<memory::AudioPacket, AudioPacketPoolAllocator::Deleter> UniqueAudioPacket;

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

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator)
{
    return UniqueAudioPacket(makePacket(allocator), allocator.getDeleter());
}

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator, const void* data, size_t length)
{
    return UniqueAudioPacket(makePacket(allocator, data, length), allocator.getDeleter());
}

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator, const Packet& packet)
{
    return UniqueAudioPacket(makePacket(allocator, packet.get(), packet.getLength()), allocator.getDeleter());
}

} // namespace memory
