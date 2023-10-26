#include "rtp/JitterTracker.h"
#include "utils/Time.h"
#include <algorithm>

namespace rtp
{
JitterTracker::JitterTracker(uint32_t rtpFrequency)
    : _prevReceiveTime(0),
      _rtpFrequency(rtpFrequency),
      _prevRtpTimestamp(0),
      _jitter(0)
{
}

void JitterTracker::update(uint64_t receiveTime, uint32_t rtpTimestamp)
{
    if (_prevReceiveTime == 0 && _prevRtpTimestamp == 0)
    {
        _prevReceiveTime = receiveTime;
        _prevRtpTimestamp = rtpTimestamp;
        return;
    }

    const int32_t deltaReceive =
        static_cast<int64_t>(receiveTime - _prevReceiveTime) * _rtpFrequency / utils::Time::sec;
    const int32_t deltaTransmit = static_cast<int32_t>(rtpTimestamp - _prevRtpTimestamp);

    _jitter += (std::abs(deltaReceive - deltaTransmit) - _jitter) / 16;
    _jitter = std::min(_jitter, static_cast<int32_t>(_rtpFrequency * 3)); // cap at 3s
    _jitter = std::max(0, _jitter);
    _prevReceiveTime = receiveTime;
    _prevRtpTimestamp = rtpTimestamp;
}

uint32_t JitterTracker::get() const
{
    return _jitter;
}

void JitterTracker::setRtpFrequency(uint32_t frequency)
{
    _rtpFrequency = frequency;
}

} // namespace rtp
