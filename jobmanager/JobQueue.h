#pragma once

#include "concurrency/Semaphore.h"
#include "jobmanager/Job.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
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
        _noNeedToRecover.test_and_set();
    }

    template <typename JOB_TYPE, typename... U>
    bool addJob(U&&... args)
    {
        static_assert(sizeof(JOB_TYPE) <= maxJobSize, "JOB_TYPE has to be <= JobQueue::maxJobSize");

        auto jobArea = _jobPool.allocate();
        if (!jobArea)
        {
            if (needToRecover())
            {
                startProcessing();
            }

            return false;
        }
        auto job = new (jobArea) JOB_TYPE(std::forward<U>(args)...);
        if (!_jobQueue.push(job))
        {
            if (needToRecover())
            {
                startProcessing();
            }
            job->~MultiStepJob();
            _jobPool.free(job);
            return false;
        }

        auto count = _jobCount.fetch_add(1);
        if (count == 0 || needToRecover())
        {
            startProcessing();
        }
        return true;
    }

    ~JobQueue()
    {
        concurrency::Semaphore sema;
        addJob<StopJob>(sema, _running);

        if (WorkerThread::isWorkerThread())
        {
            for (bool running = true; running;)
            {
                const bool jobProcessed = WorkerThread::yield();
                running = !sema.wait(jobProcessed ? 0 : 1);
            }
        }
        else
        {
            sema.wait();
        }
    }

    template <class Callable>
    bool post(Callable&& callable)
    {
        return addJob<CallableJob<std::decay_t<Callable>>>(std::forward<Callable>(callable));
    }

    template <class Callable>
    bool post(std::atomic_uint32_t& jobsCounter, Callable&& callable)
    {
        return addJob<CallableCountedJob<std::decay_t<Callable>>>(jobsCounter, std::forward<Callable>(callable));
    }

    JobManager& getJobManager() { return _jobManager; }
    size_t getCount() const { return _jobQueue.size(); }

private:
    void startProcessing()
    {
        if (!_jobManager.addJob<RunJob>(*this))
        {
            _noNeedToRecover.clear();
        }
    }

    inline bool needToRecover() { return !_noNeedToRecover.test_and_set(); }

    class RunJob : public jobmanager::MultiStepJob
    {
    public:
        explicit RunJob(JobQueue& owner) : _owner(owner) {}
        ~RunJob()
        {
            if (_actualWork)
            {
                freeJob();
            }
        }

        bool runStep() override
        {
            if (_actualWork)
            {
                auto runAgain = _actualWork->runStep();
                if (runAgain)
                {
                    return true;
                }
                freeJob();
            }

            for (; _processedCount < 10 && _owner._jobQueue.pop(_actualWork); ++_processedCount)
            {
                auto runAgain = _actualWork->runStep();
                if (runAgain)
                {
                    return true;
                }
                else
                {
                    if (_owner._running)
                    {
                        freeJob();
                    }
                    else
                    {
                        // must be in ~JobQueue. Do not touch jobqueue after sem release
                        assert(_owner._jobQueue.empty());
                        _owner._jobPool.free(_actualWork);
                        _actualWork->~MultiStepJob();
                        _actualWork = nullptr;
                        return false;
                    }
                }
            }

            auto count = _owner._jobCount.fetch_sub(_processedCount);
            if (count > _processedCount)
            {
                _owner.startProcessing();
            }

            return false;
        }

    private:
        void freeJob()
        {
            _actualWork->~MultiStepJob();
            _owner._jobPool.free(_actualWork);
            _actualWork = nullptr;
        }

        uint32_t _processedCount = 0;
        JobQueue& _owner;
        MultiStepJob* _actualWork = nullptr;
    };

    friend class RunJob;

    struct StopJob : public jobmanager::MultiStepJob
    {
        explicit StopJob(concurrency::Semaphore& sema, bool& runFlag) : _sema(sema), _running(runFlag) {}
        ~StopJob() { _sema.post(); }

        bool runStep() override
        {
            _running = false;
            return false;
        }

        concurrency::Semaphore& _sema;
        bool& _running;
    };

private:
    static const auto maxJobSize = JobManager::maxJobSize;

    JobManager& _jobManager;

    std::atomic_flag _noNeedToRecover = ATOMIC_FLAG_INIT;
    std::atomic_uint32_t _jobCount;
    bool _running;

    concurrency::MpmcQueue<MultiStepJob*> _jobQueue;
    memory::PoolAllocator<maxJobSize> _jobPool;
};

} // namespace jobmanager
