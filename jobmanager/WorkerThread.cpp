#include "jobmanager/WorkerThread.h"
#include "concurrency/ThreadUtils.h"
#include "jobmanager/JobManager.h"

namespace
{

thread_local jobmanager::WorkerThread* workerThreadHandler = nullptr;

} // namespace

namespace jobmanager
{

WorkerThread::WorkerThread(jobmanager::JobManager& jobManager)
    : _running(true),
      _jobManager(jobManager),
      _backgroundJobs{0},
      _backgroundJobMax(0),
      _backgroundJobCount(0),
      _thread([this] { this->run(); })
{
}

WorkerThread::~WorkerThread()
{
    for (auto& job : _backgroundJobs)
    {
        if (job)
        {
            _jobManager.freeJob(job);
        }
    }
}

void WorkerThread::stop()
{
    _running = false;
    _thread.join();
}

uint32_t WorkerThread::processBackgroundJobs()
{
    uint32_t pendingJobCount = 0;
    for (uint32_t i = 0; i < _backgroundJobMax; ++i)
    {
        if (_backgroundJobs[i])
        {
            const bool runAgain = _backgroundJobs[i]->runStep();
            if (runAgain)
            {
                ++pendingJobCount;
            }
            else
            {
                _jobManager.freeJob(_backgroundJobs[i]);
                _backgroundJobs[i] = nullptr;
            }
        }
    }

    if (pendingJobCount == 0)
    {
        _backgroundJobMax = 0;
    }

    return pendingJobCount;
}

void WorkerThread::run()
{
    concurrency::setThreadName("Worker");
    workerThreadHandler = this;

    try
    {
        const int64_t maxWait2ms = 64 << 15;
        int64_t pollInterval = 64;

        while (_running)
        {
            auto jobProcessed = processJobs();
            if (!jobProcessed || _backgroundJobCount == _backgroundJobs.size())
            {
                pollInterval = std::min(maxWait2ms, pollInterval * 2);
                utils::Time::nanoSleep(pollInterval);
            }
        }
    }
    catch (const std::exception& e)
    {
        logger::error("std exception %s", "WorkerThread", e.what());
    }
    catch (...)
    {
        logger::error("unknown exception", "WorkerThread");
    }
    workerThreadHandler = nullptr;
}

// return true if any new jobs were processed
bool WorkerThread::processJobs()
{
    if (_backgroundJobCount == _backgroundJobs.size())
    {
        _backgroundJobCount = processBackgroundJobs();
        if (_backgroundJobCount == _backgroundJobs.size())
        {
            // do not take on more jobs as we cannot store them if in case they are be multi-step job
            return false;
        }
    }

    uint32_t processedJobs = 0;
    for (processedJobs = 0; processedJobs < 10; ++processedJobs)
    {
        auto job = _jobManager.pop();
        if (!job)
        {
            break;
        }

        bool runAgain = job->runStep();
        if (!runAgain)
        {
            _jobManager.freeJob(job);
        }
        else
        {
            for (uint32_t i = 0; i < _backgroundJobs.size(); ++i)
            {
                if (!_backgroundJobs[i])
                {
                    _backgroundJobs[i] = job;
                    _backgroundJobMax = std::max(i + 1, _backgroundJobMax);
                    break;
                }
            }
        }
    }
    _backgroundJobCount = processBackgroundJobs();

    return processedJobs > 0;
}

bool WorkerThread::yield()
{
    WorkerThread* wt = workerThreadHandler;
    if (wt)
    {
        return wt->processJobs();
    }

    return false;
}

bool WorkerThread::isWorkerThread()
{
    return workerThreadHandler != nullptr;
}

} // namespace jobmanager
