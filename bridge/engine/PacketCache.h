#pragma once

#include "concurrency/MpmcHashmap.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/RefCountedPacket.h"

namespace bridge
{

class PacketCache
{
public:
    explicit PacketCache(const char* loggableId);
    explicit PacketCache(const char* loggableId, const uint32_t ssrc);
    ~PacketCache();

    bool add(const memory::Packet& packet, const uint16_t sequenceNumber);
    memory::RefCountedPacket* get(const uint16_t sequenceNumber);

private:
    logger::LoggableId _loggableId;

#if DEBUG
    std::atomic_uint32_t _reentrancyCounter;
#endif

    concurrency::MpmcHashmap32<uint16_t, memory::RefCountedPacket> _cache;
    std::unique_ptr<memory::PacketPoolAllocator> _packetAllocator;
    concurrency::MpmcQueue<uint16_t> _arrivalQueue;
};

} // namespace bridge
