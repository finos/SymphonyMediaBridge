#pragma once
#include <atomic>
#include <thread>
namespace concurrency
{

struct MutexGuard
{
    MutexGuard() : entryCount(0) {}

    std::atomic_uint32_t entryCount;
    std::thread::id owner;
};

// Checks that single thread is operating behind the guard
// reentrant, but exclusive.
class ScopedMutexGuard
{
public:
    explicit ScopedMutexGuard(MutexGuard& mutexBlock) : _mutexBlock(mutexBlock)
    {
        auto previousCount = mutexBlock.entryCount.fetch_add(1);
        if (previousCount == 0)
        {
            mutexBlock.owner = std::this_thread::get_id();
        }
        else
        {
            assert(mutexBlock.owner == std::this_thread::get_id());
        }
    }

    ~ScopedMutexGuard() { _mutexBlock.entryCount.fetch_sub(1); }

private:
    MutexGuard& _mutexBlock;
};
} // namespace concurrency