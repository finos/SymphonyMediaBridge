#pragma once
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
namespace memory
{

struct RefCountedPacket
{
    struct ScopedRef
    {
        explicit ScopedRef(RefCountedPacket* refCountedPacket) : _refCountedPacket(refCountedPacket) {}

        ~ScopedRef()
        {
            if (_refCountedPacket)
            {
                _refCountedPacket->release();
            }
        }

        RefCountedPacket* _refCountedPacket;
    };

    RefCountedPacket(memory::Packet* packet, memory::PacketPoolAllocator* allocator) : _allocator(allocator)
    {
        _refCount.store(1, std::memory_order_relaxed);
        _packet.store(packet, std::memory_order_relaxed);
    }

    bool retain() { return _refCount.fetch_add(1) != 0; }

    void release()
    {
        if (!_allocator || !_packet)
        {
            return;
        }

        if (_refCount.fetch_sub(1) == 1)
        {
            _allocator->free(_packet.load(std::memory_order_relaxed));
            _packet = nullptr;
        }
    }

    memory::Packet* get() { return _packet.load(std::memory_order_relaxed); }

private:
    std::atomic_uint32_t _refCount;
    std::atomic<memory::Packet*> _packet;
    memory::PacketPoolAllocator* _allocator;
};

} // namespace memory
