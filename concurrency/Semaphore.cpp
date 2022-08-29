#include "concurrency/Semaphore.h"
#include "utils/Time.h"
#include <cassert>

namespace concurrency
{

Semaphore::Semaphore(int32_t initial) : _count(initial) {}

void Semaphore::wait()
{
    std::unique_lock<std::mutex> locker(_lock);
    while (_count <= 0)
    {
        _conditionVariable.wait(locker);
    }
    _count.fetch_sub(1);
}

// returns false on timeout
bool Semaphore::wait(uint32_t timeoutMs)
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

    _count.fetch_sub(1);
    return true;
}

// reduce count by one without waiting
// use this if you know count > 0
void Semaphore::decrement()
{
    _count.fetch_sub(1);
}

void Semaphore::reset(int32_t count)
{
    std::unique_lock<std::mutex> locker(_lock);
    _count = count;
}

void Semaphore::post()
{
    std::lock_guard<std::mutex> locker(_lock);
    const auto count = _count.fetch_add(1);
    if (count >= 0)
    {
        _conditionVariable.notify_one();
    }
}

} // namespace concurrency
