#pragma once
#include "EventSemaphore.h"
#include <queue>

namespace concurrency
{

template <typename T, size_t MAX_SIZE>
class LockFullQueue
{
public:
    LockFullQueue() {}

    bool pop(T& target)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.size() == 0)
        {
            return false;
        }
        target = _queue.front();
        _queue.pop();
        if (_queue.empty())
        {
            _hasItems.reset();
        }
        return true;
    }

    bool push(T&& obj)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.size() >= MAX_SIZE)
        {
            return false;
        }
        _queue.push(obj);
        _hasItems.post();
        return true;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queue.size();
    }

    bool empty() const { return size() == 0; }

    void waitForItems(int timeoutMs) { _hasItems.await(timeoutMs); }

private:
    std::queue<T> _queue;
    mutable std::mutex _mutex;
    mutable concurrency::EventSemaphore _hasItems;
};

}