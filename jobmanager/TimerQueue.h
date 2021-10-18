#pragma once
#include "concurrency/MpmcQueue.h"
#include "concurrency/Semaphore.h"
#include <algorithm>
#include <thread>
#include <vector>
namespace jobmanager
{
class JobManager;
class Job;

// thread safe
class TimerQueue
{
public:
    TimerQueue(JobManager& jobManager, size_t maxElements);
    ~TimerQueue();

    bool addTimer(uint32_t groupId, uint32_t id, uint64_t timeoutNs, Job* job);
    bool addTimer(uint32_t groupId, uint64_t timeoutNs, Job* job, uint32_t& newId);
    void abortTimer(uint32_t groupId, uint32_t id);
    void abortTimers(uint32_t groupId);
    bool replaceTimer(uint32_t groupId, uint32_t id, uint64_t timeoutNs, Job* job);
    void stop();

private:
    struct TimerEntry
    {
        uint64_t endTime; // ns
        uint32_t id;
        uint32_t groupId;
        Job* job;

        TimerEntry() : endTime(0), id(0), groupId(0), job(nullptr) {}

        TimerEntry(uint64_t endTime, uint32_t id, uint32_t groupId, Job* jobItem)
            : endTime(endTime),
              id(id),
              groupId(groupId),
              job(jobItem)
        {
        }

        bool operator<(const TimerEntry& b) const
        {
            return (static_cast<int64_t>(b.endTime - endTime) < 0); // sort heap to nearest end
        }
    };
    struct ChangeTimer
    {
        enum Type
        {
            add,
            removeSingle,
            removeGroup
        };
        Type type;
        TimerEntry entry;

        ChangeTimer() : type(add) {}

        ChangeTimer(Type _type, const TimerEntry& _entry) : type(_type), entry(_entry) {}
    };

    bool wait(uint64_t timeoutNs, TimerEntry& entry);
    void run();
    void changeTimer(ChangeTimer& timerJob);
    bool popTimer(TimerEntry& entry);
    uint64_t getInternalTime();

    JobManager& _jobManager;
    std::vector<TimerEntry> _timers; // std::heap
    concurrency::MpmcQueue<ChangeTimer> _newTimers;
    std::atomic_uint32_t _idCounter;
    std::atomic<bool> _running;
    const uint64_t _timeReference;
    std::thread _thread; // must be last
};

} // namespace jobmanager