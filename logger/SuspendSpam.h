#pragma once

#include "utils/Time.h"

namespace logger
{

// helper to prune a log to every Nth second
class SuspendSpam
{
public:
    SuspendSpam(uint32_t passThroughLimit, uint64_t logAfterDurationNs = 0)
        : _passThrough(passThroughLimit),
          _blockDuration(std::max(utils::Time::ms * 10, logAfterDurationNs)),
          _logCount(0),
          _logTimestamp(0)
    {
    }

    bool canLog(uint64_t timestamp)
    {
        if (_logCount < _passThrough || utils::Time::diffGE(_logTimestamp, timestamp, _blockDuration))
        {
            ++_logCount;
            _logTimestamp = timestamp;
            return true;
        }

        ++_logCount;
        return false;
    }

private:
    uint32_t _passThrough;
    uint64_t _blockDuration;
    uint32_t _logCount;
    uint64_t _logTimestamp;
};

} // namespace logger
