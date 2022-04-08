#pragma once

#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PoolAllocator.h"

namespace memory
{

const size_t packetPoolSize = 2048 * 4;

using PacketPoolAllocator = PoolAllocator<sizeof(Packet)>;

inline Packet* makePacket(PacketPoolAllocator& allocator)
{
    auto pointer = allocator.allocate();
    assert(pointer);
    if (!pointer)
    {
        logger::error("Unable to allocate packet, no space left in pool %s",
            "PacketPoolAllocator",
            allocator.getName().c_str());
        return nullptr;
    }

    return new (pointer) memory::Packet();
}

inline Packet* makePacket(PacketPoolAllocator& allocator, const void* data, size_t length)
{
    assert(length <= memory::Packet::size);
    if (length > memory::Packet::size)
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

inline Packet* makePacket(PacketPoolAllocator& allocator, const Packet& packet)
{
    return makePacket(allocator, packet.get(), packet.getLength());
}

// Be very careful with reset as the deleter is not changed if already set. You may try to deallocate
// packet in the wrong pool.
typedef std::unique_ptr<memory::Packet, PacketPoolAllocator::Deleter> PacketPtr;

inline PacketPtr makePacketPtr(PacketPoolAllocator& allocator)
{
    auto packet = makePacket(allocator);
    if (!packet)
    {
        return PacketPtr();
    }

    return PacketPtr(packet, allocator.getDeleter());
}

inline PacketPtr makePacketPtr(PacketPoolAllocator& allocator, const void* data, size_t length)
{
    auto packet = makePacket(allocator, data, length);
    if (!packet)
    {
        return PacketPtr();
    }

    return PacketPtr(packet, allocator.getDeleter());
}

inline PacketPtr makePacketPtr(PacketPoolAllocator& allocator, const Packet& packet)
{
    return makePacketPtr(allocator, packet.get(), packet.getLength());
}

} // namespace memory
