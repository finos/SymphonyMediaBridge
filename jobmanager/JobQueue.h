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
          _jobCount(0),
          _running(true),
          _jobQueue(poolSize),
          _jobPool(poolSize, "SerialJobPool")
    {
        _needToRecover.test_and_set();
    }

    template <typename JOB_TYPE, typename... U>
    bool addJob(U&&... args)
    {
        static_assert(sizeof(JOB_TYPE) <= maxJobSize, "JOB_TYPE has to be <= JobQueue::maxJobSize");

        auto jobArea = _jobPool.allocate();
        if (!jobArea)
        {
            return false;
        }
        auto job = new (jobArea) JOB_TYPE(std::forward<U>(args)...);
        if (!_jobQueue.push(job))
        {
            if (!_needToRecover.test_and_set())
            {
                startProcessing();
            }
            job->~Job();
            _jobPool.free(job);
            return false;
        }

        auto count = _jobCount.fetch_add(1);
        if (count == 0 || !_needToRecover.test_and_set())
        {
            startProcessing();
        }
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
    void startProcessing()
    {
        if (!_jobManager.addJob<RunJob>(this))
        {
            _needToRecover.clear();
        }
    }

    struct RunJob : public jobmanager::Job
    {
        explicit RunJob(JobQueue* owner) : _owner(owner) {}

        void run() override { _owner->run(); }

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

    void run()
    {
        uint32_t processedCount = 0;
        for (Job* job = nullptr; processedCount < 10 && _jobQueue.pop(job); ++processedCount)
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

        auto count = _jobCount.fetch_sub(processedCount);
        if (count > processedCount)
        {
            startProcessing();
        }
    }

private:
    static const auto maxJobSize = JobManager::maxJobSize;

    JobManager& _jobManager;

    std::atomic_flag _needToRecover = ATOMIC_FLAG_INIT;
    std::atomic_uint32_t _jobCount;
    bool _running;

    concurrency::MpmcQueue<Job*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
};

} // namespace jobmanager
