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

class JobManager
{
public:
    JobManager() : _jobQueue(poolSize), _jobPool(poolSize, "JobManagerPool"), _running(true), _timers(*this, 4096 * 8)
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
        return addJobItem(static_cast<Job*>(job));
    }

    template <typename JOB_TYPE, typename... U>
    bool addTimedJob(uint32_t groupId, uint32_t id, uint64_t timeoutUs, U&&... args)
    {
        auto job = allocateJob<JOB_TYPE>(std::forward<U>(args)...);
        if (!job)
        {
            return false;
        }

        if (!_timers.addTimer(groupId, id, timeoutUs * 1000, static_cast<Job*>(job)))
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

        if (!_timers.replaceTimer(groupId, id, timeoutUs * 1000, static_cast<Job*>(job)))
        {
            freeJob(job);
            return false;
        }

        return true;
    }

    bool addJobItem(Job* job)
    {
        if (!_jobQueue.push(job))
        {
            assert(false);
            freeJob(job);
            return false;
        }
        return true;
    }

    void freeJob(Job* job)
    {
        assert(job);
        job->~Job();
        _jobPool.free(job);
    }

    Job* wait()
    {
        const int maxWait2ms = 15;
        for (int i = 0; _running.load(std::memory_order::memory_order_relaxed); i = std::min(maxWait2ms, i + 1))
        {
            Job* job;
            if (_jobQueue.pop(job))
            {
                return job;
            }
            else
            {
                utils::Time::nanoSleep(int64_t(64) << i);
            }
        }
        return nullptr;
    }

    void stop()
    {
        _timers.stop();
        _running = false;
    }

    int32_t getCount() const { return _jobQueue.size(); }

    void abortTimedJobs(const uint64_t groupId) { _timers.abortTimers(groupId); }
    void abortTimedJob(const uint64_t groupId, const uint32_t id) { _timers.abortTimer(groupId, id); }

    static const auto poolSize = 4096 * 8;
    static const auto maxJobSize = 14 * 8;

private:
    concurrency::MpmcQueue<Job*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
    std::atomic<bool> _running;

    TimerQueue _timers;
};

} // namespace jobmanager
