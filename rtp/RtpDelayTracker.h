#pragma once
#include <cinttypes>

namespace rtp
{
/**
 * Tracks arrival delay based on RTP timestamp. This measurement can be used in various filters, max trackers, avg
 * windows to decide jitter buffer levels
 *
 * Clock skew compensation default is for 48kHz rtp frequency and gives 0.5ms / s
 */
class RtpDelayTracker
{
public:
    RtpDelayTracker(uint32_t rtpFrequency, uint32_t clockSkewRtpTicks = 24);

    uint64_t update(uint64_t receiveTime, uint32_t rtpTimestamp);

    uint64_t getDelay() const;

    void reset();

    uint32_t toRtpTimestamp(uint64_t timestamp) const;
    uint32_t getFrequency() const { return _frequency; }

private:
    const uint32_t _frequency;
    const uint32_t _clockSkewCompensation;

    uint64_t _renderTime;
    uint32_t _rtpTimestamp;

    uint64_t _delay;
};

} // namespace rtp
