#include <cstdint>

namespace rtp
{
class RtcpIntervalCalculator
{
public:
    RtcpIntervalCalculator(uint32_t minIntervalMs, uint32_t maxIntervalMs);

    void setBandwidth(uint32_t bps);
    void onRtcpSent(uint32_t packetSize);
    uint32_t getIntervalMs() const;

private:
    double _avgPacketSize;
    uint32_t _rtcpBandwidthBps;
    bool _initial;
    double _intervalMs;
    double _minIntervalMs;
    double _maxIntervalMs;
};

} // namespace rtp