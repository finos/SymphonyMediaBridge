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

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator)
{
    auto pointer = allocator.allocate();
    assert(pointer);
    if (!pointer)
    {
        logger::error("Unable to allocate packet, no space left in pool %s",
            "AudioPacketPoolAllocator",
            allocator.getName().c_str());
        return UniqueAudioPacket();
    }

    new (pointer) memory::AudioPacket();

    return UniqueAudioPacket(reinterpret_cast<memory::AudioPacket*>(pointer), allocator.getDeleter());
}

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator, const void* data, size_t length)
{
    assert(length <= memory::AudioPacket::size);
    if (length > memory::AudioPacket::size)
    {
        return UniqueAudioPacket();
    }

    auto packet = makeUniquePacket(allocator);
    if (!packet)
    {
        return UniqueAudioPacket();
    }

    std::memcpy(packet->get(), data, length);
    packet->setLength(length);
    return packet;
}

inline UniqueAudioPacket makeUniquePacket(AudioPacketPoolAllocator& allocator, const Packet& packet)
{
    return makeUniquePacket(allocator, packet.get(), packet.getLength());
}

} // namespace memory
