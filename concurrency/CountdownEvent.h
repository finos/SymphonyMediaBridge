#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace concurrency
{

/**
 * Event becomes set when counter reaches 0.
 * Event is clear when counter is > 0.
 * Like an inverted semaphore.
 */
class CountdownEvent
{
public:
    CountdownEvent(uint32_t initial = 0);
    ~CountdownEvent() = default;

    void wait();
    bool wait(const uint32_t timeoutMs);
    CountdownEvent& operator++();
    CountdownEvent& operator--();
    void reset(uint32_t count = 0);

    uint32_t getCount() const;

private:
    mutable std::mutex _lock;
    std::condition_variable _conditionVariable;
    uint32_t _count;
};

} // namespace concurrency
