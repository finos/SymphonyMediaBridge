#pragma once

#include <cassert>
#include <cstdint>
#include <limits>

namespace utils
{

/** Pacer for nanosecond interval. Keeps track of next interval tick. */
class Pacer
{
public:
    explicit Pacer(int64_t intervalNanoSeconds) : _intervalNanoseconds(intervalNanoSeconds), _nextTick(0)
    {
        assert(_intervalNanoseconds > 0);
    }

    void tick(uint64_t timestamp)
    {
        int64_t latency = timeDiff(timestamp, _nextTick);
        if (latency > 3 * _intervalNanoseconds || latency < -_intervalNanoseconds * 2)
        {
            reset(timestamp);
            return;
        }

        if (latency > -_intervalNanoseconds / 2)
        {
            _nextTick += _intervalNanoseconds;
        }
    }

    // in nanoseconds
    int64_t timeToNextTick(uint64_t timestamp) const
    {
        if (_nextTick > timestamp)
        {
            assert(_nextTick - timestamp <= std::numeric_limits<int64_t>::max());
        }
        else if (timestamp > _nextTick)
        {
            assert(timestamp - _nextTick <= std::numeric_limits<int64_t>::max());
        }

        return static_cast<int64_t>(_nextTick - timestamp);
    }

    void reset(uint64_t timestamp) { _nextTick = timestamp + _intervalNanoseconds; }

private:
    const int64_t _intervalNanoseconds;
    uint64_t _nextTick;

    int64_t timeDiff(uint64_t a, uint64_t b) const
    {
        if (a > b)
        {
            assert(a - b <= std::numeric_limits<int64_t>::max());
        }
        else if (b > a)
        {
            assert(b - a <= std::numeric_limits<int64_t>::max());
        }

        return static_cast<int64_t>(a - b);
    }
};

} // namespace utils