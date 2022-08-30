#pragma once
#include "concurrency/DebtSemaphore.h"
#include "concurrency/WaitFreeStack.h"
#include "utils/Time.h"
#include <array>

namespace emulator
{

class TimeTurner : public utils::TimeSource
{
public:
    static const size_t MAX_THREAD_COUNT = 30;

    TimeTurner();

    virtual uint64_t getAbsoluteTime() override { return _timestamp; }
    virtual void nanoSleep(uint64_t nanoSeconds) override;

    virtual std::chrono::system_clock::time_point wallClock() override;

    void shutdown();

    void advance();
    void advance(uint64_t nanoSeconds);
    void waitForThreadsToSleep(uint32_t expectedCount, uint64_t timeoutNs);
    void runFor(uint64_t durationNs);

    TimeTurner& operator+=(uint64_t nanoSeconds)
    {
        advance(nanoSeconds);
        return *this;
    }

private:
    uint64_t _timestamp;

    std::chrono::system_clock::time_point _startTime;

    enum State : uint32_t
    {
        Empty = 0,
        Allocated,
        Committed,
        Fired
    };

    struct Sleeper
    {
        Sleeper() : state(State::Empty), expireTimestamp(0) {}

        std::atomic<State> state;
        uint64_t expireTimestamp;
        concurrency::DebtSemaphore semaphore;
    };

    std::array<Sleeper, MAX_THREAD_COUNT> _sleepers;
    bool _running;

    // equals 1 when all threads are asleep.
    concurrency::DebtSemaphore _sleeperCount;
};
} // namespace emulator
