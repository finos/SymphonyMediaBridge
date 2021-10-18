#pragma once
#include <cstdint>

namespace bridge
{

class PacketCache;

class RecordingOutboundContext
{
public:
    RecordingOutboundContext(PacketCache& packetCache) : _sequenceNumber(0), _packetCache(packetCache) {}

    uint16_t _sequenceNumber;
    PacketCache& _packetCache;
};

} // namespace bridge
