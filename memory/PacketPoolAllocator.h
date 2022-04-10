#pragma once

#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PoolAllocator.h"

namespace memory
{

const size_t packetPoolSize = 2048 * 4;

using PacketPoolAllocator = PoolAllocator<sizeof(Packet)>;

// Be very careful with reset as the deleter is not changed if already set. You may try to deallocate
// packet in the wrong pool.
typedef std::unique_ptr<memory::Packet, PacketPoolAllocator::Deleter> UniquePacket;

inline UniquePacket makeUniquePacket(PacketPoolAllocator& allocator)
{
    auto pointer = allocator.allocate();
    assert(pointer);
    if (!pointer)
    {
        logger::error("Unable to allocate packet, no space left in pool %s",
            "PacketPoolAllocator",
            allocator.getName().c_str());
        return UniquePacket();
    }

    new (pointer) memory::Packet();

    return UniquePacket(reinterpret_cast<Packet*>(pointer), allocator.getDeleter());
}

inline UniquePacket makeUniquePacket(PacketPoolAllocator& allocator, const void* data, size_t length)
{
    assert(length <= memory::Packet::size);
    if (length > memory::Packet::size)
    {
        return UniquePacket();
    }

    auto packet = makeUniquePacket(allocator);
    if (!packet)
    {
        return UniquePacket();
    }

    std::memcpy(packet->get(), data, length);
    packet->setLength(length);

    return packet;
}

inline UniquePacket makeUniquePacket(PacketPoolAllocator& allocator, const Packet& packet)
{
    return makeUniquePacket(allocator, packet.get(), packet.getLength());
}

} // namespace memory
