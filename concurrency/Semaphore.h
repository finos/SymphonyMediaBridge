#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace concurrency
{

/**
 * Semaphore that allows negative counter. Waiting thread is released if count > 0.
 */
class Semaphore
{
public:
    Semaphore(int32_t initial = 0);
    ~Semaphore() = default;

    void wait();
    bool wait(const uint32_t timeoutMs);
    void post();
    void reset(int32_t count = 0);

    void decrement();

    int32_t getCount() const { return _count.load(); }

private:
    std::mutex _lock;
    std::condition_variable _conditionVariable;
    std::atomic_int32_t _count;
};

} // namespace concurrency
