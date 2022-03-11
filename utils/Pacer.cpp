#include "utils/Pacer.h"

namespace utils
{

int64_t Pacer::timeToNextTick(uint64_t timestamp) const
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

} // namespace utils
