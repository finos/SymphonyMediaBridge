#include "FakeCrossTraffic.h"
#include "utils/Time.h"
namespace fakenet
{
FakeCrossTraffic::FakeCrossTraffic(memory::PacketPoolAllocator& allocator, uint32_t mtu, uint32_t bandwidthKbps)
    : _allocator(allocator),
      _releaseTime(0),
      _bandwidthKbps(bandwidthKbps),
      _mtu(mtu)
{
}

memory::Packet* FakeCrossTraffic::getPacket(uint64_t timestamp)
{
    if (_bandwidthKbps == 0)
    {
        return nullptr;
    }
    if (_releaseTime == 0)
    {
        _releaseTime = timestamp;
    }

    if (utils::Time::diff(_releaseTime, timestamp) >= 0)
    {
        auto* packet = memory::makePacket(_allocator);
        packet->get()[0] = CROSS_TRAFFIC_PROTOCOL;
        packet->setLength(randomPacketSize());
        const auto deltaTime = utils::Time::sec * _mtu / (_bandwidthKbps * int64_t(125));
        _releaseTime += deltaTime;
        return packet;
    }
    return nullptr;
}

int64_t FakeCrossTraffic::timeToRelease(uint64_t timestamp) const
{
    return std::max(int64_t(0), utils::Time::diff(timestamp, _releaseTime));
}

void FakeCrossTraffic::setBandwidth(uint32_t kbps)
{
    _bandwidthKbps = kbps;
}

size_t FakeCrossTraffic::randomPacketSize()
{
    return _mtu;
}

} // namespace fakenet