#pragma once

#include "utils/Time.h"
#include <atomic>
#include <chrono>

namespace concurrency
{

class ScopedSpinLocker
{
public:
    explicit ScopedSpinLocker(std::atomic_flag& lock) : _lock(lock)
    {
        while (_lock.test_and_set(std::memory_order_acquire))
        {
        }
        _hasLock = true;
    }

    explicit ScopedSpinLocker(std::atomic_flag& lock, const std::chrono::nanoseconds& timeout) : _lock(lock)
    {
        if (timeout == std::chrono::nanoseconds::zero())
        {
            _hasLock = !_lock.test_and_set(std::memory_order_acquire);
            return;
        }

        auto start = utils::Time::getAbsoluteTime();
        bool value = true;
        for (value = _lock.test_and_set(std::memory_order_acquire);
             value && std::chrono::nanoseconds(utils::Time::getAbsoluteTime() - start) < timeout;)
        {
            value = _lock.test_and_set(std::memory_order_acquire);
        }
        _hasLock = !value;
    }

    bool hasLock() const { return _hasLock; }

    ~ScopedSpinLocker()
    {
        if (_hasLock)
        {
            _lock.clear(std::memory_order_release);
        }
    }

private:
    std::atomic_flag& _lock;
    bool _hasLock;
};

}
