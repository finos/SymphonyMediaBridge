#include "TimerQueue.h"
#include "JobManager.h"
#include "concurrency/ThreadUtils.h"

namespace jobmanager
{

TimerQueue::TimerQueue(size_t maxElements)
    : _newTimers(maxElements),
      _idCounter(0),
      _running(true),
      _timeReference(utils::Time::getAbsoluteTime()),
      _thread([this] { this->run(); })
{
}

TimerQueue::~TimerQueue()
{
    stop();
    _thread.join();
}

// multi threaded
bool TimerQueue::addTimer(uint32_t groupId,
    uint64_t timeoutNs,
    MultiStepJob& job,
    JobManager& jobManager,
    uint32_t& newId)
{
    const uint32_t id = _idCounter++;

    if (addTimer(groupId, id, timeoutNs, job, jobManager))
    {
        newId = id;
        return true;
    }
    return false;
}

bool TimerQueue::addTimer(uint32_t groupId, uint32_t id, uint64_t timeoutNs, MultiStepJob& job, JobManager& jobManager)
{
    return _newTimers.push(
        ChangeTimer(ChangeTimer::add, TimerEntry(getInternalTime() + timeoutNs, id, groupId, &job, &jobManager)));
}

bool TimerQueue::replaceTimer(uint32_t groupId,
    uint32_t id,
    uint64_t timeoutNs,
    MultiStepJob& job,
    JobManager& jobManager)
{
    abortTimer(groupId, id);
    return addTimer(groupId, id, timeoutNs, job, jobManager);
}

void TimerQueue::abortTimer(uint32_t groupId, uint32_t id)
{
    _newTimers.push(ChangeTimer(ChangeTimer::removeSingle, TimerEntry(0, id, groupId, nullptr, nullptr)));
}

void TimerQueue::abortTimers(uint32_t groupId)
{
    _newTimers.push(ChangeTimer(ChangeTimer::removeGroup, TimerEntry(0, 0, groupId, nullptr, nullptr)));
}

void TimerQueue::stop()
{
    _running = false;
}

void TimerQueue::run()
{
    concurrency::setThreadName("TimerQueue");
    TimerEntry entry;
    while (_running.load(std::memory_order::memory_order_relaxed))
    {
        ChangeTimer timerJob;
        while (_newTimers.pop(timerJob))
        {
            changeTimer(timerJob);
        }

        if (!_timers.empty())
        {
            auto firstTimer = _timers.front();
            int64_t toWait = firstTimer.endTime - getInternalTime();
            if (toWait <= 0)
            {
                popTimer(entry);
                entry.jobManager->addJobItem(entry.job);
            }
            else
            {
                utils::Time::nanoSleep(std::min(1 * utils::Time::ms, static_cast<uint64_t>(toWait)));
            }
        }
        else
        {
            utils::Time::nanoSleep(1 * utils::Time::ms);
        }
    }

    ChangeTimer nEntry;
    while (_newTimers.pop(nEntry))
    {
        if (nEntry.type == ChangeTimer::add)
        {
            nEntry.entry.jobManager->freeJob(nEntry.entry.job);
        }
    }

    while (popTimer(entry))
    {
        entry.jobManager->freeJob(entry.job);
    }
}

bool TimerQueue::popTimer(TimerEntry& entry)
{
    if (_timers.empty())
    {
        return false;
    }
    entry = _timers.front();
    if (_timers.size() == 1)
    {
        _timers.clear();
        return true;
    }
    std::pop_heap(_timers.begin(), _timers.end());
    _timers.pop_back();
    return true;
}

void TimerQueue::changeTimer(ChangeTimer& timerJob)
{
    if (timerJob.type == ChangeTimer::add)
    {
        _timers.push_back(timerJob.entry);
        std::push_heap(_timers.begin(), _timers.end());
    }
    else if (timerJob.type == ChangeTimer::removeSingle)
    {
        for (auto it = _timers.begin(); it != _timers.end();)
        {
            if (it->id == timerJob.entry.id && it->groupId == timerJob.entry.groupId)
            {
                auto entry = *it;
                it = _timers.erase(it);
                entry.jobManager->freeJob(entry.job);
                std::make_heap(_timers.begin(), _timers.end());
                return;
            }
            else
                ++it;
        }
    }
    else if (timerJob.type == ChangeTimer::removeGroup)
    {
        bool modified = false;
        for (auto it = _timers.begin(); it != _timers.end();)
        {
            if (it->groupId == timerJob.entry.groupId)
            {
                auto entry = *it;
                it = _timers.erase(it);
                modified = true;
                entry.jobManager->freeJob(entry.job);
            }
            else
            {
                ++it;
            }
        }
        if (modified)
        {
            std::make_heap(_timers.begin(), _timers.end());
        }
    }
}

// used to avoid wrapping of endtime in TimeEntries in case absolute time does not start at 0
// This gives us 584y up time before it happens
inline uint64_t TimerQueue::getInternalTime()
{
    return utils::Time::getAbsoluteTime() - _timeReference;
}

} // namespace jobmanager
