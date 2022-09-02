#pragma once

#include "utils/Time.h"
#include <algorithm>

namespace logger
{

// helper to limit log to every Nth log.
class PruneSpam
{
public:
    PruneSpam(uint32_t passThroughLimit, uint32_t logEveryNth)
        : _passThrough(passThroughLimit),
          _logEvery(std::max(1u, logEveryNth)),
          _logCount(0)
    {
    }

    bool canLog()
    {
        if (_logCount < _passThrough || (_logCount % _logEvery) == 0)
        {
            ++_logCount;
            return true;
        }

        ++_logCount;
        return false;
    }

private:
    uint32_t _passThrough;
    uint32_t _logEvery;
    uint32_t _logCount;
};

} // namespace logger
