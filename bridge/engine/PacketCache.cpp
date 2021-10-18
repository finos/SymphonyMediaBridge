#include "bridge/engine/PacketCache.h"
#include "memory/RefCountedPacket.h"
#include "utils/ScopedReentrancyBlocker.h"

namespace
{

const size_t maxPackets = 512;

}

namespace bridge
{
PacketCache::PacketCache(const char* loggableId)
    : _loggableId(loggableId),
#if DEBUG
      _reentrancyCounter(0),
#endif
      _cache(maxPackets * 2),
      _packetAllocator(std::make_unique<memory::PacketPoolAllocator>(maxPackets * 2, _loggableId.c_str())),
      _arrivalQueue(maxPackets * 2)
{
    logger::info("Creating cache", _loggableId.c_str());
}

PacketCache::PacketCache(const char* loggableId, const uint32_t ssrc)
    : _loggableId(loggableId),
#if DEBUG
      _reentrancyCounter(0),
#endif
      _cache(maxPackets * 2),
      _packetAllocator(std::make_unique<memory::PacketPoolAllocator>(maxPackets * 2, _loggableId.c_str())),
      _arrivalQueue(maxPackets * 2)
{
    logger::info("Creating cache for ssrc %u", _loggableId.c_str(), ssrc);
}

PacketCache::~PacketCache()
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
#endif

    uint16_t removedSequenceNumberEntry;
    while (_arrivalQueue.pop(removedSequenceNumberEntry))
    {
        auto removedPacketItr = _cache.find(removedSequenceNumberEntry);
        if (removedPacketItr == _cache.end())
        {
            continue;
        }

        _packetAllocator->free(removedPacketItr->second.get());
        _cache.erase(removedSequenceNumberEntry);
    }
}

bool PacketCache::add(memory::Packet* packet, const uint16_t sequenceNumber)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
#endif

    if (_cache.contains(sequenceNumber))
    {
        return false;
    }

    if (_arrivalQueue.size() >= maxPackets)
    {
        uint16_t removedSequenceNumberEntry;
        const auto popResult = _arrivalQueue.pop(removedSequenceNumberEntry);
        assert(popResult);

        auto removedPacketItr = _cache.find(removedSequenceNumberEntry);
        if (removedPacketItr != _cache.end())
        {
            removedPacketItr->second.release();
            _cache.erase(removedSequenceNumberEntry);
        }
    }

    auto cachedPacket = memory::makePacket(*_packetAllocator, *packet);
    if (!cachedPacket)
    {
        return false;
    }

    _cache.emplace(sequenceNumber, cachedPacket, _packetAllocator.get());

    _arrivalQueue.push(sequenceNumber);
    return true;
}

memory::RefCountedPacket* PacketCache::get(const uint16_t sequenceNumber)
{
    auto cacheItr = _cache.find(sequenceNumber);
    if (cacheItr == _cache.end())
    {
        return nullptr;
    }

    if (cacheItr->second.retain())
    {
        return &cacheItr->second;
    }

    return nullptr;
}

} // namespace bridge
