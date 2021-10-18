#pragma once
#include <cstdint>

namespace memory
{
class Packet;
}

namespace fakenet
{

// meta data put at end of memory::Packet in fake network
struct PacketMetaData
{
    uint64_t sendTime;
};

PacketMetaData& getMetaData(memory::Packet& packet);

class MediaSource
{
public:
    virtual ~MediaSource(){};
    virtual memory::Packet* getPacket(uint64_t timestamp) = 0;
    virtual int64_t timeToRelease(uint64_t timestamp) const = 0;
    virtual void setBandwidth(uint32_t kbps) = 0;
    virtual uint32_t getBandwidth() const = 0;
};

uint32_t randomSize(uint32_t originalSize, double ratio);

} // namespace fakenet