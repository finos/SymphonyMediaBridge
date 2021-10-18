#include "memory/RingAllocator.h"
#include "utils/Time.h"
#include <array>
#include <cinttypes>

namespace bwe
{

class BwBurstTracker
{
public:
    BwBurstTracker();

    void onPacketReceived(uint32_t packetSize, uint64_t receiveTime);
    double getBandwidthPercentile(double percentile);

private:
    struct PacketAggregate
    {
        PacketAggregate() : receiveTime(0), size(0) {}
        PacketAggregate(uint16_t size_, uint64_t receivetime_) : receiveTime(receivetime_), size(size_) {}

        uint64_t receiveTime = 0;
        uint16_t size = 0;
    };

    std::array<PacketAggregate, 256> _window;
    size_t _head;
    size_t _tail;
    std::array<double, 32> _buckets;
    double _bucketTotal;
    double _previousReport;
    uint64_t _previousReceiveTime;
    uint32_t _count;

    static const uint32_t LIMIT_FIRST_BUCKET = 500;
};
} // namespace bwe