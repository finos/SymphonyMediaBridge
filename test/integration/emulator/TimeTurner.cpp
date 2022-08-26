#include "TimeTurner.h"

namespace emulator
{

TimeTurner::TimeTurner()
    : _timestamp(100),
      _startTime(std::chrono::system_clock::now()),
      _running(true),
      _sleepersCount(0)
{
}

void TimeTurner::nanoSleep(const uint64_t nanoSeconds)
{
    if (!_running)
    {
        utils::Time::rawNanoSleep(nanoSeconds);
        return;
    }

    _sleepersCount.fetch_add(1);

    const auto timerExpires = _timestamp.load() + nanoSeconds;
    for (size_t i = 0; i < _sleepers.size() * 5; ++i)
    {
        auto& slot = _sleepers[i % _sleepers.size()];
        auto expectedState = State::Empty;
        if (slot.state.compare_exchange_strong(expectedState, State::Allocated))
        {
            slot.expireTimestamp = timerExpires;
            slot.state.store(State::Committed);

            slot.semaphore.wait();
            slot.state.store(State::Empty);
            _sleepersCount.fetch_sub(1);
            return;
        }
    }
    assert(false);
}

std::chrono::system_clock::time_point TimeTurner::wallClock()
{
    return _startTime +
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(_timestamp.load()));
}

/**
 * Requires all threads to be created and asleep. You cannot remove or add threads.
 */
void TimeTurner::runFor(uint64_t nanoSeconds)
{
    const auto startTime = getAbsoluteTime();

    const auto threadCount = _sleepersCount.load();
    while (getAbsoluteTime() - startTime < nanoSeconds)
    {
        advance();
        do
        {
            utils::Time::rawNanoSleep(10);
        } while (_sleepersCount.load() < threadCount);
    }
}

void TimeTurner::advance()
{
    int64_t minDuration = std::numeric_limits<int64_t>::max();
    const auto timestamp = _timestamp.load();

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
    const auto timestamp = _timestamp.load() + nanoSeconds;
    _timestamp = timestamp;

    for (auto& slot : _sleepers)
    {
        if (slot.state.load() == State::Committed && utils::Time::diffGE(slot.expireTimestamp, timestamp, 0))
        {
            slot.state = State::Fired;
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
