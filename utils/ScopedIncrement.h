#pragma once

#include <atomic>

namespace utils
{

class ScopedIncrement
{
public:
    explicit ScopedIncrement(std::atomic_uint32_t& counter) : _counter(counter) { ++_counter; }

    ~ScopedIncrement() { --_counter; }

    std::atomic_uint32_t& getCounter() { return _counter; }

private:
    std::atomic_uint32_t& _counter;
};

}
