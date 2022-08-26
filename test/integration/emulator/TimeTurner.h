#pragma once
#include "concurrency/Semaphore.h"
#include "concurrency/WaitFreeStack.h"
#include "utils/Time.h"
#include <array>

namespace emulator
{

class TimeTurner : public utils::TimeSource
{
public:
    TimeTurner();

    virtual uint64_t getAbsoluteTime() override { return _timestamp; }
    virtual void nanoSleep(uint64_t nanoSeconds) override;

    virtual std::chrono::system_clock::time_point wallClock() override;

    void shutdown();

    void advance();
    void advance(uint64_t nanoSeconds);
    void runFor(uint64_t nanoSeconds);

    TimeTurner& operator+=(uint64_t nanoSeconds)
    {
        advance(nanoSeconds);
        return *this;
    }

private:
    std::atomic_uint64_t _timestamp;

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
        concurrency::Semaphore semaphore;
    };

    std::array<Sleeper, 30> _sleepers;
    bool _running;
    std::atomic_uint32_t _sleepersCount;
};
} // namespace emulator
