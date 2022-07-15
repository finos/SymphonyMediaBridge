#pragma once

#include <atomic>
#include <cassert>

namespace utils
{

class ScopedIncrement
{
public:
    explicit ScopedIncrement(std::atomic_uint32_t& counter) : _counter(counter) { ++_counter; }

    ~ScopedIncrement()
    {
#if DEBUG
        auto value = --_counter;
        assert(value != 0xFFFFFFFFu);
#else
        --_counter;
#endif
    }

private:
    std::atomic_uint32_t& _counter;
};

} // namespace utils
