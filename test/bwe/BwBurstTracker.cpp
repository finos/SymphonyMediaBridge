#include "BwBurstTracker.h"

#include "utils/Time.h"

namespace bwe
{

BwBurstTracker::BwBurstTracker() : _start(0), _totalSize(0), _count(0) {}

void BwBurstTracker::onPacketReceived(uint32_t packetSize, uint64_t receiveTime, double delayMs, uint32_t queueSize)
{
    if (_start == 0 || queueSize < packetSize * 8 + 2000)
    {
        _start = receiveTime - std::min(delayMs, 20.0) * utils::Time::ms;
        _totalSize = 0;
        _count = 0;
    }

    _totalSize += packetSize;
    ++_count;
}

double BwBurstTracker::getBurstBandwidthKbps(uint64_t timestamp) const
{
    if (timestamp - _start < utils::Time::ms * 5)
    {
        return 0;
    }

    return _totalSize * 8 * utils::Time::ms / (timestamp - _start);
}

} // namespace bwe