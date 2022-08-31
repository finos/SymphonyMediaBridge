#include "concurrency/CountdownEvent.h"
#include "utils/Time.h"
#include <cassert>

namespace concurrency
{

CountdownEvent::CountdownEvent(uint32_t initial) : _count(initial) {}

void CountdownEvent::wait()
{
    std::unique_lock<std::mutex> locker(_lock);
    while (_count > 0)
    {
        _conditionVariable.wait(locker, [this]() { return this->_count == 0; });
    }
}

// returns false on timeout
bool CountdownEvent::wait(uint32_t timeoutMs)
{
    auto timeoutAt = utils::Time::getAbsoluteTime() + timeoutMs * 1000000ull;
    std::unique_lock<std::mutex> locker(_lock);
    while (_count > 0)
    {
        int64_t timeLeftMs = std::max(int64_t(0), int64_t(timeoutAt - utils::Time::getAbsoluteTime())) / 1000000ll;
        if (std::cv_status::timeout == _conditionVariable.wait_for(locker, std::chrono::milliseconds(timeLeftMs)))
        {
            return false;
        }
    }

    return true;
}

void CountdownEvent::reset(uint32_t count)
{
    std::unique_lock<std::mutex> locker(_lock);
    _count = count;
    if (_count == 0)
    {
        _conditionVariable.notify_all();
    }
}

CountdownEvent& CountdownEvent::operator++()
{
    std::lock_guard<std::mutex> locker(_lock);
    ++_count;
    return *this;
}

CountdownEvent& CountdownEvent::operator--()
{
    std::lock_guard<std::mutex> locker(_lock);
    if (_count > 0)
    {
        --_count;
    }

    if (_count == 0)
    {
        _conditionVariable.notify_all();
    }
    return *this;
}

uint32_t CountdownEvent::getCount() const
{
    std::lock_guard<std::mutex> locker(_lock);
    return _count;
}

} // namespace concurrency
