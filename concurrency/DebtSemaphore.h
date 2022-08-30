#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace concurrency
{

/**
 * Semaphore that allows negative counter. Waiting thread is released if count > 0.
 */
class DebtSemaphore
{
public:
    DebtSemaphore(int32_t initial = 0);
    ~DebtSemaphore() = default;

    void wait();
    bool wait(const uint32_t timeoutMs);
    void post();
    void reset(int32_t count = 0);

    void decrement();

    int32_t getCount() const;

private:
    mutable std::mutex _lock;
    std::condition_variable _conditionVariable;
    int32_t _count;
};

} // namespace concurrency
