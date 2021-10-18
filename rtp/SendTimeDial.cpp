#include "SendTimeDial.h"
#include "utils/Time.h"

namespace rtp
{
SendTimeDial::SendTimeDial() : _timeReference(0), _sendTimeReference(0), _initialized(false) {}

// send time is a 24 bit fixed point with 6bit mantissa
// Every 512 send time cycle will the two time lines align perfectly (tick).
uint64_t SendTimeDial::toAbsoluteTime(uint32_t sendTime6_18, uint64_t timestamp)
{
    const int64_t localTick = utils::Time::sec / 512;
    const int32_t sendTimeTick = (1 << 18) / 512;

    if (!_initialized)
    {
        _timeReference = timestamp - (timestamp % localTick) - 2 * localTick;
        _sendTimeReference = sendTime6_18 - (sendTime6_18 % sendTimeTick);
        _initialized = true;
        return _timeReference + localTick * (sendTime6_18 - _sendTimeReference) / sendTimeTick;
    }

    const auto localAdvance = timestamp - _timeReference;
    if (localAdvance > utils::Time::sec)
    {
        uint64_t ticks = localAdvance / localTick;
        _timeReference += ticks * localTick;
        _sendTimeReference = (_sendTimeReference + ticks * sendTimeTick) % (1 << 24);
    }

    auto sendTimeAdvance = static_cast<int32_t>(sendTime6_18 - _sendTimeReference);
    if (sendTimeAdvance <= -(1 << 23))
    {
        sendTimeAdvance += (1 << 24);
    }
    else if (sendTimeAdvance > (1 << 23))
    {
        sendTimeAdvance -= (1 << 24);
    }

    return _timeReference + localTick * sendTimeAdvance / sendTimeTick;
}

} // namespace rtp