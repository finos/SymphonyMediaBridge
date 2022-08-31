#include "concurrency/Semaphore.h"
#include "utils/Time.h"
#include <cassert>

namespace concurrency
{

Semaphore::Semaphore(uint32_t initial) : _count(initial) {}

void Semaphore::wait()
{
    std::unique_lock<std::mutex> locker(_lock);
    while (_count == 0)
    {
        _conditionVariable.wait(locker);
    }
    --_count;
}

// returns false on timeout
bool Semaphore::wait(uint32_t timeoutMs)
{
    auto timeoutAt = utils::Time::getAbsoluteTime() + timeoutMs * 1000000ull;
    std::unique_lock<std::mutex> locker(_lock);
    while (_count == 0)
    {
        int64_t timeLeftMs = std::max(int64_t(0), int64_t(timeoutAt - utils::Time::getAbsoluteTime())) / 1000000ll;
        if (std::cv_status::timeout == _conditionVariable.wait_for(locker, std::chrono::milliseconds(timeLeftMs)))
        {
            return false;
        }
    }

    --_count;
    return true;
}

void Semaphore::reset()
{
    std::unique_lock<std::mutex> locker(_lock);
    _count = 0;
}

void Semaphore::post()
{
    std::lock_guard<std::mutex> locker(_lock);
    ++_count;
    _conditionVariable.notify_one();
}

} // namespace concurrency
