#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace concurrency
{

class Semaphore
{
public:
    Semaphore(uint32_t initial = 0);
    ~Semaphore() = default;

    void wait();
    bool wait(const uint32_t timeoutMs);
    void post();
    void reset();

private:
    mutable std::mutex _lock;
    std::condition_variable _conditionVariable;
    uint32_t _count;
};

} // namespace concurrency
