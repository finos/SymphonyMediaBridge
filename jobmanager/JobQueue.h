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

class JobQueue
{
public:
    explicit JobQueue(JobManager& jobManager, size_t poolSize = 4096)
        : _jobManager(jobManager),
          _running(true),
          _jobQueue(poolSize),
          _jobPool(poolSize - 1, "SerialJobPool")
    {
        _runJobPosted.clear();
    }

    template <typename JOB_TYPE, typename... U>
    bool addJob(U&&... args)
    {
        static_assert(sizeof(JOB_TYPE) <= maxJobSize, "JOB_TYPE has to be <= JobQueue::maxJobSize");

        auto jobArea = _jobPool.allocate();
        if (!jobArea)
        {
            ensurePosted();
            return false;
        }
        auto job = new (jobArea) JOB_TYPE(std::forward<U>(args)...);
        if (!_jobQueue.push(reinterpret_cast<Job*>(job)))
        {
            job->~Job();
            _jobPool.free(job);
            ensurePosted();
            return false;
        }

        ensurePosted();
        return true;
    }

    ~JobQueue()
    {
        concurrency::Semaphore sema;
        addJob<StopJob>(sema, _running);
        sema.wait();
    }

    JobManager& getJobManager() { return _jobManager; }
    size_t getCount() const { return _jobQueue.size(); }

private:
    struct RunJob : public jobmanager::Job
    {
        explicit RunJob(JobQueue* owner) : _owner(owner) {}

        void run() override { _owner->run(*this); }

        JobQueue* _owner;
    };

    struct StopJob : public jobmanager::Job
    {
        explicit StopJob(concurrency::Semaphore& sema, bool& runFlag) : _sema(sema), _running(runFlag) {}
        ~StopJob() { _sema.post(); }

        void run() override { _running = false; }

        concurrency::Semaphore& _sema;
        bool& _running;
    };

    void run(RunJob& runJob)
    {
        Job* job;

        for (int jobCount = 0; jobCount < 10 && _jobQueue.pop(job); ++jobCount)
        {
            job->run();
            if (_running)
            {
                job->~Job();
                _jobPool.free(job);
            }
            else
            {
                assert(_jobQueue.empty());
                _jobPool.free(job);
                job->~Job(); // semaphore is set and we cannot touch JobQueue anymore
                return;
            }
        }

        _runJobPosted.clear();
        if (!_jobQueue.empty())
        {
            ensurePosted();
        }
    }

private:
    void ensurePosted()
    {
        if (!_runJobPosted.test_and_set())
        {
            if (!_jobManager.addJob<RunJob>(this))
            {
                _runJobPosted.clear();
                // no RunJob posted!
                // hope for next job to post a RunJob.
            }
        }
    }

    static const auto maxJobSize = JobManager::maxJobSize;

    JobManager& _jobManager;

    std::atomic_flag _runJobPosted;
    bool _running;

    concurrency::MpmcQueue<Job*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
};

} // namespace jobmanager
