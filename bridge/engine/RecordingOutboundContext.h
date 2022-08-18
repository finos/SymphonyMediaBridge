#pragma once
#include <cstdint>

namespace bridge
{

class PacketCache;

class RecordingOutboundContext
{
public:
    RecordingOutboundContext(PacketCache& packetCache) : sequenceNumber(0), packetCache(packetCache) {}

    uint16_t sequenceNumber;
    PacketCache& packetCache;
};

} // namespace bridge
