#pragma once

#include "jobmanager/JobQueue.h"
#include "utils/Time.h"
#include <functional>
#include <vector>

namespace jobmanager
{

class AsyncWaitTask
{
public:
    virtual ~AsyncWaitTask() = default;

    virtual bool canComplete() = 0;
    virtual void onComplete() = 0;
    virtual void onTimeout() = 0;
};

class AsyncWaiter
{

    constexpr static uint32_t timerGroupId = 23963832U;

public:
    struct AsyncEntry
    {
        AsyncEntry(std::unique_ptr<AsyncWaitTask>&& task, uint64_t startTime, uint64_t endTime)
            : task(std::move(task)),
              startTime(startTime),
              endTime(endTime)
        {
        }

        std::unique_ptr<AsyncWaitTask> task;
        uint64_t startTime;
        uint64_t endTime;
    };

    struct CheckJob : CountedJob
    {
        CheckJob(AsyncWaiter& asyncWaiter) : CountedJob(asyncWaiter._jobCounter), _asyncWaiter(asyncWaiter) {}

        void run() final { _asyncWaiter.runCheck(); }

        AsyncWaiter& _asyncWaiter;
    };

    struct ScheduleCheckJobTimer : CountedJob
    {
        ScheduleCheckJobTimer(AsyncWaiter& asyncWaiter) : CountedJob(asyncWaiter._jobCounter), _asyncWaiter(asyncWaiter)
        {
        }

        void run() final { _asyncWaiter._jobQueue.template addJob<CheckJob>(_asyncWaiter); }

        AsyncWaiter& _asyncWaiter;
    };

    struct AddTaskJob : CountedJob
    {
        template <class... TaskArgs>
        AddTaskJob(AsyncWaiter& asyncWaiter, uint64_t timeout, std::unique_ptr<AsyncWaitTask>&& task)
            : CountedJob(asyncWaiter._jobCounter),
              _asyncWaiter(asyncWaiter),
              _task(std::move(task)),
              _timeout(timeout)
        {
        }

        void run() final { _asyncWaiter.addNewTaskInternal(_timeout, std::move(_task)); }

        AsyncWaiter& _asyncWaiter;
        std::unique_ptr<AsyncWaitTask> _task;
        uint64_t _timeout;
    };

public:
    explicit AsyncWaiter(JobManager& jobManager, size_t maxCheckInterval, size_t jobQueueSize = 128);

    template <class Task, class... Args>
    void emplaceTask(uint64_t timeout, Args&&... args);
    template <class Task>
    void addTask(uint64_t timeout, Task&& task);

    bool hasTaskWaiting() const { return _jobCounter.load() != 0; }

private:
    void runCheck();
    void scheduleNexCheck(uint64_t delay);
    void addNewTaskInternal(uint64_t timeout, std::unique_ptr<AsyncWaitTask>&& task);
    bool hasCompleted(AsyncWaitTask& task);

private:
    JobQueue _jobQueue;
    std::vector<AsyncEntry> _tasks;
    std::atomic_uint32_t _jobCounter;
    uint64_t _maxCheckInterval;
    uint64_t _timerId;
};

inline AsyncWaiter::AsyncWaiter(JobManager& jobManager, size_t maxCheckInterval, size_t jobQueueSize)
    : _jobQueue(jobManager, jobQueueSize),
      _tasks(),
      _jobCounter(0),
      _maxCheckInterval(maxCheckInterval),
      _timerId(reinterpret_cast<uint64_t>(this))
{
}

inline void AsyncWaiter::runCheck()
{
    logger::info("runCheck", "AsyncWaiter");
    const auto timestamp = utils::Time::getAbsoluteTime();
    auto itEnd = _tasks.end();
    auto nextTimeout = _maxCheckInterval;
    for (auto it = _tasks.begin(); it != itEnd;)
    {
        const bool hasCompleted = this->hasCompleted(*it->task);
        const bool isEnded = hasCompleted || timestamp >= it->endTime;
        if (isEnded)
        {
            logger::info("isEnded", "AsyncWaiter");
            if (!hasCompleted)
            {
                it->task->onTimeout();
            }

            *it = std::move(*(itEnd - 1));
            --itEnd;
        }
        else
        {
            nextTimeout = std::min(nextTimeout, it->endTime - timestamp);
            ++it;
        }
    }

    _tasks.erase(itEnd, _tasks.end());
    if (!_tasks.empty())
    {
        scheduleNexCheck(nextTimeout);
    }
}

inline void AsyncWaiter::addNewTaskInternal(uint64_t timeout, std::unique_ptr<AsyncWaitTask>&& task)
{
    const bool hasCompleted = this->hasCompleted(*task);
    if (!hasCompleted)
    {
        const auto startTime = utils::Time::getAbsoluteTime();
        _tasks.emplace_back(std::move(task), startTime, startTime + timeout);
        if (_tasks.size() == 1)
        {
            const auto delay = std::min(timeout, _maxCheckInterval);
            scheduleNexCheck(delay);
        }
    }
}

inline bool AsyncWaiter::hasCompleted(AsyncWaitTask& task)
{
    if (task.canComplete())
    {
        task.onComplete();
        return true;
    }

    return false;
}

inline void AsyncWaiter::scheduleNexCheck(uint64_t delay)
{
    _jobQueue.getJobManager().template replaceTimedJob<ScheduleCheckJobTimer>(timerGroupId,
        _timerId,
        delay / 1000,
        *this);
}

template <class Task>
void AsyncWaiter::addTask(uint64_t timeout, Task&& task)
{
    emplaceTask<Task>(timeout, std::forward<Task>(task));
}

template <class Task, class... Args>
void AsyncWaiter::emplaceTask(uint64_t timeout, Args&&... args)
{
    static_assert(std::is_base_of<AsyncWaitTask, Task>::value, "Task type should be inherit from AsyncWaitTask");
    _jobQueue.addJob<AddTaskJob>(*this, timeout, std::make_unique<Task>(std::forward<Args>(args)...));
}

} // namespace jobmanager