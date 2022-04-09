#pragma once
#include "FakeMedia.h"

namespace fakenet
{
class FakeCrossTraffic : public MediaSource
{
public:
    FakeCrossTraffic(memory::PacketPoolAllocator& allocator, uint32_t mtu, uint32_t bandwidthKbps);

    memory::UniquePacket getPacket(uint64_t timestamp) override;
    int64_t timeToRelease(uint64_t timestamp) const override;
    void setBandwidth(uint32_t kbps) override;
    uint32_t getBandwidth() const override { return _bandwidthKbps; }
    static const char CROSS_TRAFFIC_PROTOCOL = 64; // to distinguish from stun, dtls, rtp
    uint32_t getSsrc() const override { return 0; }

private:
    size_t randomPacketSize();

    memory::PacketPoolAllocator& _allocator;
    uint64_t _releaseTime;
    uint32_t _bandwidthKbps;
    uint32_t _mtu;
};

} // namespace fakenet
