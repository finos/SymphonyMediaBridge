#pragma once

#include <atomic>
#include <cassert>

namespace utils
{

class ScopedReentrancyBlocker
{
public:
    explicit ScopedReentrancyBlocker(std::atomic_uint32_t& counter) : _counter(counter)
    {
        auto oldValue = _counter.fetch_add(1);
        assert(oldValue == 0);
        oldValue = 0;
    }

    ~ScopedReentrancyBlocker() { _counter.fetch_sub(1); }

private:
    std::atomic_uint32_t& _counter;
};

} // namespace utils
