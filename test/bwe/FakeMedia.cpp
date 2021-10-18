#include "FakeMedia.h"
#include "memory/Packet.h"
#include <cstdlib>

namespace fakenet
{

PacketMetaData& getMetaData(memory::Packet& packet)
{
    return reinterpret_cast<PacketMetaData&>(packet.get()[memory::Packet::size - sizeof(PacketMetaData)]);
}

uint32_t randomSize(uint32_t originalSize, double ratio)
{
    const auto maxDiff = static_cast<uint32_t>(originalSize * ratio);
    return originalSize - maxDiff / 2 + rand() % maxDiff;
}

} // namespace fakenet
