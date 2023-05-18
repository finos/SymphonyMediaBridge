#pragma once
#include <cinttypes>

namespace rtp
{

/**
 * Track RTP jitter according to RFC 3550
 */
class JitterTracker
{
public:
    JitterTracker(uint32_t rtpFrequency);

    void setRtpFrequency(uint32_t frequency);
    void update(uint64_t receiveTime, uint32_t rtpTimestamp);
    uint32_t get() const;
    uint32_t getRtpFrequency() const { return _rtpFrequency; }

private:
    uint64_t _prevReceiveTime;
    uint32_t _rtpFrequency;
    uint32_t _prevRtpTimestamp;
    int32_t _jitter;
};

} // namespace rtp
