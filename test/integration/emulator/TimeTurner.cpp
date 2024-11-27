#include "TimeTurner.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include <thread>

namespace emulator
{

TimeTurner::TimeTurner(uint64_t granularity)
    : _timestamp(100),
      _startTime(std::chrono::system_clock::now()),
      _running(true),
      _abort(false),
      _granularity(granularity)
{
}

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

#if 0
            char threadName[90];
            size_t nameLength = 89;
            concurrency::getThreadName(threadName, nameLength);
            logger::debug("%s woke up", "TimeTurner", threadName);
#endif

            slot.state.store(State::Empty);
            return;
        }
    }

    assert(false);
}

std::chrono::system_clock::time_point TimeTurner::wallClock() const
{
    return _startTime +
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(_timestamp.load()));
}

/**
 * You can run this initially if you know how many threads minimum will join the sleep.
 * It can only be run once.
 */
void TimeTurner::waitForThreadsToSleep(uint32_t expectedCount, uint64_t timeoutNs)
{
    const int MAX_ITERATIONS = 1000;
    const auto interval = timeoutNs / MAX_ITERATIONS;
    uint32_t count = 0;
    uint32_t visitors = 0;

    for (int i = 0; i < MAX_ITERATIONS; ++i)
    {
        count = 0;
        visitors = 0;
        for (auto& slot : _sleepers)
        {
            if (slot.state.load() == State::Sleeping)
            {
                ++count;
            }
            if (slot.state.load() != State::Empty)
            {
                ++visitors;
            }
        }
        if (count == expectedCount)
        {
            logger::info("%u threads asleep as expected. Visitors %u", "TimeTurner", count, visitors);
            return;
        }

        utils::Time::rawNanoSleep(interval);
    }
    logger::warn("%u threads asleep. Expected %u. Visitors %u", "TimeTurner", count, expectedCount, visitors);
}

/**
 * Requires all threads to be created and asleep when called. You cannot remove or add threads after calling runFor
 * or while runFor runs.
 */
void TimeTurner::runFor(uint64_t durationNs)
{
    const auto startTime = getAbsoluteTime();

    for (auto timestamp = startTime; !_abort && utils::Time::diffLE(startTime, timestamp, durationNs);
         timestamp = getAbsoluteTime())
    {
        logger::awaitLogDrained(0.75);
        // wait up to 1s for threads to go into nanoSleep
        for (int i = 0; i < 200; ++i)
        {
            if (_sleeperCountdown.wait(5))
            {
                break;
            }

            // release worker threads to unlock the wait in case it is a semaphore they are waiting for
            advance(16);
        }

        if (_sleeperCountdown.getCount() > 0)
        {
            logger::warn("Timeout waiting for threads to sleep %u", "TimeTurner", _sleeperCountdown.getCount());

            if (!_running)
            {
                _abortSemaphore.post();
                return;
            }
        }
        advance();
    }

    _sleeperCountdown.wait();
}

void TimeTurner::stop()
{
    _abort = true;
}

void TimeTurner::advance()
{
    int64_t minDuration = std::numeric_limits<int64_t>::max();
    const auto timestamp = _timestamp.load();

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
    nanoSeconds = std::max(nanoSeconds, _granularity);
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
