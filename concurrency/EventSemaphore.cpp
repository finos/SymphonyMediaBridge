#include "concurrency/EventSemaphore.h"
#include <chrono>

using namespace std::chrono_literals;

namespace concurrency
{

void EventSemaphore::post()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _flag = 1;
    _condition.notify_all();
}

bool EventSemaphore::await(int32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);
    return _condition.wait_for(lock, timeoutMs * 1ms, [this]() { return this->_flag == 1; });
}

void EventSemaphore::await()
{
    std::unique_lock<std::mutex> lock(_mutex);
    _condition.wait(lock, [this]() { return this->_flag == 1; });
}

void EventSemaphore::reset()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _flag = 0;
}

} // namespace concurrency
