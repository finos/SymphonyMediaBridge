#pragma once

#include "concurrency/MpmcHashmap.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/RandomAccessBacklog.h"

namespace bridge
{

/**
 * @brief
 * PacketCache is not thread safe. Make sure you add, get and remove on the same thread context.
 *
 */
class PacketCache
{
public:
    explicit PacketCache(const char* loggableId);
    explicit PacketCache(const char* loggableId, const uint32_t ssrc);
    ~PacketCache();

    bool add(const memory::Packet& packet, const uint16_t sequenceNumber);
    const memory::Packet* get(const uint16_t sequenceNumber);

    constexpr static size_t maxPackets = 512;

private:
    logger::LoggableId _loggableId;

#if DEBUG
    std::atomic_uint32_t _reentrancyCounter;
#endif

    memory::PacketPoolAllocator _packetAllocator;
    concurrency::MpmcHashmap32<uint16_t, memory::UniquePacket> _cache;
    memory::RandomAccessBacklog<uint16_t, maxPackets> _arrivalQueue;
};

} // namespace bridge
