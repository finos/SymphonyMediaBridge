#pragma once
#include "concurrency/MpmcQueue.h"
#include <thread>
#include <vector>
namespace jobmanager
{
class JobManager;
class MultiStepJob;

// thread safe
class TimerQueue
{
public:
    explicit TimerQueue(size_t maxElements);
    ~TimerQueue();

    bool addTimer(uint32_t groupId, uint32_t id, uint64_t timeoutNs, MultiStepJob& job, JobManager& jobManager);
    bool addTimer(uint32_t groupId, uint64_t timeoutNs, MultiStepJob& job, JobManager& jobManager, uint32_t& newId);
    void abortTimer(uint32_t groupId, uint32_t id);
    void abortTimers(uint32_t groupId);
    bool replaceTimer(uint32_t groupId, uint32_t id, uint64_t timeoutNs, MultiStepJob& job, JobManager& jobManager);
    void stop();

private:
    struct TimerEntry
    {
        uint64_t endTime; // ns
        uint32_t id;
        uint32_t groupId;
        MultiStepJob* job;
        JobManager* jobManager;

        TimerEntry() : endTime(0), id(0), groupId(0), job(nullptr), jobManager(nullptr) {}

        TimerEntry(uint64_t endTime, uint32_t id, uint32_t groupId, MultiStepJob* jobItem, JobManager* jobManager)
            : endTime(endTime),
              id(id),
              groupId(groupId),
              job(jobItem),
              jobManager(jobManager)
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

    std::vector<TimerEntry> _timers; // std::heap
    concurrency::MpmcQueue<ChangeTimer> _newTimers;
    std::atomic_uint32_t _idCounter;
    std::atomic<bool> _running;
    const uint64_t _timeReference;
    std::thread _thread; // must be last
};

} // namespace jobmanager
