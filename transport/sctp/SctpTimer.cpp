#include "SctpTimer.h"
#include <algorithm>

namespace sctp
{

bool Timer::hasExpired(uint64_t timestamp) const
{
    if (!isRunning())
    {
        return false;
    }
    return remainingTime(timestamp) == 0;
}

void Timer::expireAt(uint64_t timestamp)
{
    _expires = timestamp;
    _armed = true;
}

void Timer::stop()
{
    _armed = false;
}

void Timer::startMs(uint64_t timestamp, uint32_t timeoutMs)
{
    _expires = timestamp + std::min(_maxTimeout, timeoutMs * timer::ms);
    _armed = true;
}

void Timer::startNs(uint64_t timestamp, uint64_t timeoutNs)
{
    _expires = timestamp + timeoutNs;
    _armed = true;
}

int64_t Timer::remainingTime(uint64_t timestamp) const
{
    if (!isRunning())
    {
        return std::numeric_limits<int64_t>::max();
    }
    return std::max(static_cast<int64_t>(_expires - timestamp), int64_t(0));
}
} // namespace sctp