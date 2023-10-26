#include "rtp/RtpDelayTracker.h"
#include "utils/Time.h"
#include <algorithm>

namespace rtp
{

RtpDelayTracker::RtpDelayTracker(uint32_t rtpFrequency, uint32_t clockSkewRtpTicks)
    : _frequency(rtpFrequency),
      _clockSkewCompensation(clockSkewRtpTicks),
      _renderTime(0),
      _rtpTimestamp(0)
{
}

uint64_t RtpDelayTracker::update(uint64_t receiveTime, uint32_t rtpTimestamp)
{
    if (_renderTime == 0 && _rtpTimestamp == 0)
    {
        _renderTime = receiveTime;
        _rtpTimestamp = rtpTimestamp;
        _delay = 0;
        return 0;
    }
    if (static_cast<int32_t>(_rtpTimestamp - rtpTimestamp) > static_cast<int32_t>(_frequency * 2))
    {
        // very long reverse, indicating a reordered packet arriving later than 2s, or an rtp timestamp reset.
        _renderTime = receiveTime;
        _rtpTimestamp = rtpTimestamp;
        return utils::Time::ms * 10;
    }

    _renderTime += static_cast<int32_t>(rtpTimestamp - _rtpTimestamp) * static_cast<int64_t>(utils::Time::sec) /
        (_frequency - _clockSkewCompensation);

    if (static_cast<int64_t>(_renderTime - receiveTime) > 0)
    {
        // cannot receive frame before being created. adjust
        _renderTime = receiveTime;
    }

    _rtpTimestamp = rtpTimestamp;

    _delay = receiveTime - _renderTime;
    if (_delay > utils::Time::ms * 10)
    {
        _renderTime += _delay * 0.01;
    }
    return _delay;
}

void RtpDelayTracker::reset()
{
    _renderTime = 0;
    _rtpTimestamp = 0;
}

uint32_t RtpDelayTracker::toRtpTimestamp(uint64_t timestamp) const
{
    return static_cast<int32_t>(static_cast<int64_t>((timestamp - _renderTime) * _frequency / utils::Time::sec)) +
        _rtpTimestamp;
}

} // namespace rtp
