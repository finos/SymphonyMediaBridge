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
            slot.state.store(State::Committed);
            _sleeperCount.post();
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
 * Requires all threads to be created and asleep when called. You cannot remove or add threads after calling runFor
 * or while runFor runs.
 */
void TimeTurner::runFor(uint64_t durationNs)
{
    _sleeperCount.reset(1);
    const auto startTime = getAbsoluteTime();

    for (auto timestamp = getAbsoluteTime(); utils::Time::diffLE(startTime, timestamp, durationNs);
         timestamp = getAbsoluteTime())
    {
        logger::awaitLogDrain();
        _sleeperCount.wait();
        _sleeperCount.post();
        advance();
    }

    _sleeperCount.wait();
}

void TimeTurner::advance()
{
    int64_t minDuration = std::numeric_limits<int64_t>::max();
    const auto timestamp = _timestamp;

    Sleeper* minItem = nullptr;
    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Committed)
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
        if (slot.state.load() == State::Committed && utils::Time::diffGE(slot.expireTimestamp, _timestamp, 0))
        {
            slot.state = State::Fired;
            _sleeperCount.decrement();
            slot.semaphore.post();
        }
    }
}

void TimeTurner::shutdown()
{
    _running = false;
    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Committed)
        {
            slot.state = State::Fired;
            slot.semaphore.post();
        }
    }
}

} // namespace emulator
