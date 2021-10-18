#pragma once

#include "concurrency/Semaphore.h"
#include "jobmanager/Job.h"
#include "jobmanager/JobManager.h"
#include "logger/Logger.h"
#include "memory/PoolAllocator.h"
#include "utils/Trackers.h"
#include <list>
#include <memory>
#include <unistd.h>
namespace jobmanager
{

class SerialJobManager
{
public:
    explicit SerialJobManager(JobManager& jobManager, size_t poolSize = 4096)
        : _jobManager(jobManager),
          _running(false),
          _jobQueue(poolSize),
          _jobPool(poolSize - 1, "SerialJobPool")
    {
        _runJobPosted.clear();
    }

    template <typename JOB_TYPE, typename... U>
    bool addJob(U&&... args)
    {
        static_assert(sizeof(JOB_TYPE) <= maxJobSize, "JOB_TYPE has to be <= SerialJobManager::maxJobSize");

        auto jobArea = _jobPool.allocate();
        if (!jobArea)
        {
            ensurePosted();
            return false;
        }
        auto job = new (jobArea) JOB_TYPE(std::forward<U>(args)...);
        if (!_jobQueue.push(reinterpret_cast<Job*>(job)))
        {
            freeJob(job);
            ensurePosted();
            return false;
        }

        ensurePosted();
        return true;
    }

    ~SerialJobManager()
    {
        concurrency::Semaphore sema;
        addJob<StopJob>(sema);
        sema.wait();
        while (_running.load())
        {
            usleep(10000);
        }
    }

    JobManager& getJobManager() { return _jobManager; }
    size_t getCount() const { return _jobQueue.size(); }

private:
    struct RunJob : public jobmanager::Job
    {
        explicit RunJob(SerialJobManager* owner) : _owner(owner) {}

        void run() override { _owner->run(*this); }

        SerialJobManager* _owner;
    };

    struct StopJob : public jobmanager::Job
    {
        explicit StopJob(concurrency::Semaphore& sema) : _sema(sema) {}

        void run() override { _sema.post(); }

        concurrency::Semaphore& _sema;
    };

    void run(RunJob& runJob)
    {
        _running.store(true);
        Job* job;

        int i = 0;
        while (i < 10 && _jobQueue.pop(job))
        {
            job->run();
            freeJob(job);
            ++i;
        }
        _runJobPosted.clear();
        if (!_jobQueue.empty())
        {
            ensurePosted();
        }

        _running.store(false);
    }

private:
    void freeJob(Job* job)
    {
        job->~Job();
        _jobPool.free(job);
    }

    void ensurePosted()
    {
        if (!_runJobPosted.test_and_set())
        {
            if (!_jobManager.addJob<RunJob>(this))
            {
                _runJobPosted.clear();
                // failed hope for next job to post a run job.
            }
        }
    }

    static const auto maxJobSize = JobManager::maxJobSize;

    JobManager& _jobManager;

    std::atomic_flag _runJobPosted;
    std::atomic_bool _running;

    concurrency::MpmcQueue<Job*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
};

} // namespace jobmanager
