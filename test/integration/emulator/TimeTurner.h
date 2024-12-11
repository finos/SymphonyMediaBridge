#pragma once
#include "concurrency/CountdownEvent.h"
#include "concurrency/EventSemaphore.h"
#include "concurrency/Semaphore.h"
#include "concurrency/WaitFreeStack.h"
#include "utils/Time.h"
#include <array>

namespace emulator
{

class TimeTurner : public utils::TimeSource
{
public:
    static const size_t MAX_THREAD_COUNT = 60;

    explicit TimeTurner(uint64_t granularity = 2 * utils::Time::ms);

    virtual uint64_t getAbsoluteTime() const override { return _timestamp; }
    virtual void nanoSleep(uint64_t nanoSeconds) override;

    virtual std::chrono::system_clock::time_point wallClock() const override;

    void shutdown();

    void advance();
    void advance(uint64_t nanoSeconds) override;
    void waitForThreadsToSleep(uint32_t expectedCount, uint64_t timeoutNs);
    void runFor(uint64_t durationNs);

    TimeTurner& operator+=(uint64_t nanoSeconds)
    {
        advance(nanoSeconds);
        return *this;
    }

    void stop();

    void resetTime(uint64_t timestamp) { _timestamp = timestamp; }

private:
    std::atomic_uint64_t _timestamp;

    const std::chrono::system_clock::time_point _startTime;

    enum State : uint32_t
    {
        Empty = 0,
        Allocated,
        Sleeping,
        Fired
    };

    struct Sleeper
    {
        Sleeper() : state(State::Empty), expireTimestamp(0) {}

        std::atomic<State> state;
        uint64_t expireTimestamp;
        concurrency::Semaphore semaphore;
    };

    std::array<Sleeper, MAX_THREAD_COUNT> _sleepers;
    std::atomic_bool _running;

    concurrency::CountdownEvent _sleeperCountdown;
    concurrency::EventSemaphore _abortSemaphore;
    std::atomic_bool _abort;
    uint64_t _granularity;
};
} // namespace emulator
