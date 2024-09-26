#include <array>
#include <cinttypes>

namespace bwe
{

class BwBurstTracker
{
public:
    BwBurstTracker();

    void onPacketReceived(uint32_t packetSize, uint64_t receiveTime, double delayMs, uint32_t queueSize);
    double getBurstBandwidthKbps(uint64_t timestamp) const;

private:
    uint64_t _start;
    uint32_t _totalSize;
    uint32_t _count;
};
} // namespace bwe