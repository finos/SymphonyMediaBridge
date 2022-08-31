#include "TimeTurner.h"
#include "logger/Logger.h"
#include <thread>

namespace emulator
{

TimeTurner::TimeTurner() : _timestamp(100), _startTime(std::chrono::system_clock::now()), _running(true) {}

void TimeTurner::nanoSleep(const uint64_t nanoSeconds)
{
    if (!_running)
    {
        std::this_thread::yield();
        return;
    }

    const auto timerExpires = _timestamp + nanoSeconds;
    for (size_t i = 0; i < _sleepers.size() * 5; ++i)
    {
        auto& slot = _sleepers[i % _sleepers.size()];
        auto expectedState = State::Empty;
        if (slot.state.compare_exchange_strong(expectedState, State::Allocated))
        {
            slot.expireTimestamp = timerExpires;
            slot.state.store(State::Sleeping);
            --_sleeperCountdown;
            slot.semaphore.wait();

            slot.state.store(State::Empty);
            return;
        }
    }

    assert(false);
}

std::chrono::system_clock::time_point TimeTurner::wallClock()
{
    return _startTime + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(_timestamp));
}

/**
 * You can run this initially if you know how many threads minimum will join the sleep.
 * It can only be run once.
 */
void TimeTurner::waitForThreadsToSleep(uint32_t expectedCount, uint64_t timeoutNs)
{
    const int MAX_ITERATIONS = 1000;
    const auto interval = timeoutNs / MAX_ITERATIONS;
    for (int i = 0; i < MAX_ITERATIONS; ++i)
    {
        uint32_t count = 0;
        for (auto& slot : _sleepers)
        {
            if (slot.state.load() == State::Sleeping)
            {
                ++count;
            }
        }
        if (count == expectedCount)
        {
            return;
        }

        utils::Time::rawNanoSleep(interval);
    }
}

/**
 * Requires all threads to be created and asleep when called. You cannot remove or add threads after calling runFor
 * or while runFor runs.
 */
void TimeTurner::runFor(uint64_t durationNs)
{
    const auto startTime = getAbsoluteTime();

    for (auto timestamp = getAbsoluteTime(); utils::Time::diffLE(startTime, timestamp, durationNs);
         timestamp = getAbsoluteTime())
    {
        logger::awaitLogDrained(0.75);
        _sleeperCountdown.wait();
        advance();
    }

    _sleeperCountdown.wait();
}

void TimeTurner::advance()
{
    int64_t minDuration = std::numeric_limits<int64_t>::max();
    const auto timestamp = _timestamp;

    Sleeper* minItem = nullptr;
    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Sleeping)
        {
            const auto expiresIn = utils::Time::diff(timestamp, slot.expireTimestamp);
            if (expiresIn < minDuration)
            {
                minItem = &slot;
                minDuration = expiresIn;
            }
        }
    }

    if (minItem)
    {
        advance(std::max(int64_t(0), minDuration));
    }
}

void TimeTurner::advance(uint64_t nanoSeconds)
{
    _timestamp += nanoSeconds;

    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Sleeping && utils::Time::diffGE(slot.expireTimestamp, _timestamp, 0))
        {
            slot.state = State::Fired;
            ++_sleeperCountdown;
            slot.semaphore.post();
        }
    }
}

void TimeTurner::shutdown()
{
    _running = false;

    uint64_t maxTimeout = 0;
    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Sleeping)
        {
            maxTimeout = std::max(maxTimeout, slot.expireTimestamp);
        }
    }
    _timestamp += maxTimeout;

    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Sleeping)
        {
            slot.state = State::Fired;
            slot.semaphore.post();
        }
    }
}

} // namespace emulator
