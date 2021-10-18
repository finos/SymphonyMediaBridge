#include "RtcpIntervalCalculator.h"
#include <algorithm>
namespace rtp
{
RtcpIntervalCalculator::RtcpIntervalCalculator(uint32_t minIntervalMs, uint32_t maxIntervalMs)
    : _avgPacketSize(200),
      _rtcpBandwidthBps(2000),
      _initial(true),
      _intervalMs(50),
      _minIntervalMs(minIntervalMs),
      _maxIntervalMs(maxIntervalMs)
{
}

void RtcpIntervalCalculator::setBandwidth(uint32_t bps)
{
    _rtcpBandwidthBps = bps;
}

void RtcpIntervalCalculator::onRtcpSent(uint32_t packetSize)
{
    const double SMOOTHING = 0.625; // smoothing
    _avgPacketSize += SMOOTHING * (packetSize - _avgPacketSize);
    auto targetInterval = _avgPacketSize * 8000. / std::max(1u, _rtcpBandwidthBps);
    if (_initial)
    {
        _intervalMs = targetInterval / 2;
        _initial = false;
    }
    else
    {
        _intervalMs += SMOOTHING * (targetInterval - _intervalMs);
    }
}

uint32_t RtcpIntervalCalculator::getIntervalMs() const
{
    return std::min(_maxIntervalMs, std::max(_minIntervalMs, _intervalMs));
}

} // namespace rtp