#include "bridge/engine/PacketCache.h"
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
      _packetAllocator(std::make_unique<memory::PacketPoolAllocator>(maxPackets, _loggableId.c_str())),
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
      _packetAllocator(std::make_unique<memory::PacketPoolAllocator>(maxPackets, _loggableId.c_str())),
      _arrivalQueue(maxPackets * 2)
{
    logger::info("Creating cache for ssrc %u", _loggableId.c_str(), ssrc);
}

PacketCache::~PacketCache()
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
#endif
    // must clear out content in case PacketAllocator is removed first.
    _cache.reInitialize();
}

bool PacketCache::add(const memory::Packet& packet, const uint16_t sequenceNumber)
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
        uint16_t sequenceNumberToRemove;
        const auto popResult = _arrivalQueue.pop(sequenceNumberToRemove);
        assert(popResult);

        auto removedPacketItr = _cache.find(sequenceNumberToRemove);
        if (removedPacketItr != _cache.end())
        {
            removedPacketItr->second.reset();
            _cache.erase(sequenceNumberToRemove);
        }
    }

    auto cachedPacket = memory::makeUniquePacket(*_packetAllocator, packet);
    if (!cachedPacket)
    {
        return false;
    }

    _cache.emplace(sequenceNumber, std::move(cachedPacket));
    _arrivalQueue.push(sequenceNumber);
    return true;
}

const memory::Packet* PacketCache::get(const uint16_t sequenceNumber)
{
    auto cacheItr = _cache.find(sequenceNumber);
    if (cacheItr != _cache.end())
    {
        return cacheItr->second.get();
    }

    return nullptr;
}

} // namespace bridge
