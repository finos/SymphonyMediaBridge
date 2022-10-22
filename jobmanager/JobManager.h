#pragma once

#include "TimerQueue.h"
#include "jobmanager/Job.h"
#include "memory/PoolAllocator.h"
#include "utils/Trackers.h"
#include <list>
#include <memory>
#include <unistd.h>

namespace jobmanager
{

/**
 * JobManager provides means for a pool of worker threads to do collaborative multitasking without overflowing the
 * system with threads.
 * @see WorkerThread. There are two ways to handle long running jobs.
 * 1. Inherit MultiStep job and return true in runStep method to indicate that job is not completed. WorkerThread will
 * process other jobs and then call runStep again on your MultiStep job.
 * 2. Call WorkerThread::yield from within your run method. That will allow the worker thread to process other jobs off
 * the main job queue and then return from yield. Note that this builds call stack.
 *
 * Note that a long running job at the front of a jobmanager::JobQueue will still block other jobs on that queue despite
 * yielding or using MultiStepJob. It is only when the jobs reach JobManager main queue that yielding cause more jobs to
 * run "simultaneously".
 *
 * JobManager facilitates timer jobs. Use addTimedJob to post a job that will be run after a specific timeout.
 * You have to handle re-triggering yourself. You can abort a specific timer by id, or a group of related timers using a
 * group id.
 *
 */
class JobManager
{
public:
    JobManager(TimerQueue& timerQueue)
        : _jobQueue(poolSize),
          _jobPool(poolSize, "JobManagerPool"),
          _running(true),
          _timers(timerQueue)
    {
    }

    template <typename JOB_TYPE, typename... U>
    JOB_TYPE* allocateJob(U&&... args)
    {
        static_assert(sizeof(JOB_TYPE) <= maxJobSize, "JOB_TYPE has to be <= JobManager::maxJobSize");

        auto jobArea = _jobPool.allocate();
        if (!jobArea)
        {
            return nullptr;
        }
        return new (jobArea) JOB_TYPE(std::forward<U>(args)...);
    }

    template <typename JOB_TYPE, typename... U>
    bool addJob(U&&... args)
    {
        auto job = allocateJob<JOB_TYPE>(std::forward<U>(args)...);
        if (!job)
        {
            return false;
        }
        return addJobItem(job);
    }

    template <typename JOB_TYPE, typename... U>
    bool addTimedJob(uint32_t groupId, uint32_t id, uint64_t timeoutUs, U&&... args)
    {
        auto job = allocateJob<JOB_TYPE>(std::forward<U>(args)...);
        if (!job)
        {
            return false;
        }

        if (!_timers.addTimer(groupId, id, timeoutUs * 1000, *job, *this))
        {
            freeJob(job);
            return false;
        }

        return true;
    }

    template <typename JOB_TYPE, typename... U>
    bool replaceTimedJob(uint32_t groupId, uint32_t id, uint64_t timeoutUs, U&&... args)
    {
        auto job = allocateJob<JOB_TYPE>(std::forward<U>(args)...);
        if (!job)
        {
            return false;
        }

        if (!_timers.replaceTimer(groupId, id, timeoutUs * 1000, *job, *this))
        {
            freeJob(job);
            return false;
        }

        return true;
    }

    bool addJobItem(MultiStepJob* job)
    {
        if (!_jobQueue.push(job))
        {
            assert(false);
            freeJob(job);
            return false;
        }
        return true;
    }

    void freeJob(MultiStepJob* job)
    {
        assert(job);
        job->~MultiStepJob();
        _jobPool.free(job);
    }

    MultiStepJob* pop()
    {
        MultiStepJob* job;
        if (_running.load(std::memory_order::memory_order_relaxed) && _jobQueue.pop(job))
        {
            return job;
        }
        else
        {
            return nullptr;
        }
    }

    void stop()
    {
        _timers.stop();
        _running = false;
    }

    int32_t getCount() const { return _jobPool.countAllocatedItems(); }

    void abortTimedJobs(const uint64_t groupId) { _timers.abortTimers(groupId); }
    void abortTimedJob(const uint64_t groupId, const uint32_t id) { _timers.abortTimer(groupId, id); }

    static const auto poolSize = 4096 * 8;
    static const auto maxJobSize = 14 * 8;

private:
    concurrency::MpmcQueue<MultiStepJob*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
    std::atomic<bool> _running;

    TimerQueue& _timers;
};

} // namespace jobmanager
