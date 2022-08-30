#include "concurrency/DebtSemaphore.h"
#include "utils/Time.h"
#include <cassert>

namespace concurrency
{

DebtSemaphore::DebtSemaphore(int32_t initial) : _count(initial) {}

void DebtSemaphore::wait()
{
    std::unique_lock<std::mutex> locker(_lock);
    while (_count <= 0)
    {
        _conditionVariable.wait(locker);
    }
    --_count;
}

// returns false on timeout
bool DebtSemaphore::wait(uint32_t timeoutMs)
{
    auto timeoutAt = utils::Time::getAbsoluteTime() + timeoutMs * 1000000ull;
    std::unique_lock<std::mutex> locker(_lock);
    while (_count <= 0)
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

void DebtSemaphore::decrement()
{
    std::unique_lock<std::mutex> locker(_lock);
    --_count;
}

void DebtSemaphore::reset(int32_t count)
{
    std::unique_lock<std::mutex> locker(_lock);
    _count = count;
}

void DebtSemaphore::post()
{
    std::lock_guard<std::mutex> locker(_lock);
    if (++_count > 0)
    {
        _conditionVariable.notify_one();
    }
}

int32_t DebtSemaphore::getCount() const
{
    std::lock_guard<std::mutex> locker(_lock);
    return _count;
}

} // namespace concurrency
