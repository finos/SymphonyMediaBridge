#include "jobmanager/WorkerThread.h"
#include "concurrency/ThreadUtils.h"
#include "jobmanager/JobManager.h"

namespace
{

thread_local jobmanager::WorkerThread* workerThreadHandler = nullptr;

} // namespace

namespace jobmanager
{

WorkerThread::WorkerThread(jobmanager::JobManager& jobManager, bool yieldEnabled, const char* name)
    : _running(true),
      _jobManager(jobManager),
      _backgroundJobCount(0),
      _yieldEnabled(yieldEnabled),
      _name(name ? name : "Worker"),
      _thread([this] { this->run(); })
{
}

WorkerThread::~WorkerThread()
{
    for (auto& backgroundJob : _backgroundJobs)
    {
        if (backgroundJob.job)
        {
            _jobManager.freeJob(backgroundJob.job);
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
    for (auto& backgroundJob : _backgroundJobs)
    {
        // running flag prevents re-entrance in case the WorkerThread yields and comes back here
        if (backgroundJob.job && !backgroundJob.running)
        {
            backgroundJob.running = true;
            const bool runAgain = backgroundJob.job->runStep();
            if (runAgain)
            {
                ++pendingJobCount;
            }
            else
            {
                _jobManager.freeJob(backgroundJob.job);
                backgroundJob.job = nullptr;
            }
            backgroundJob.running = false;
        }
    }

    if (pendingJobCount == 0)
    {
        _backgroundJobs.clear();
    }

    return pendingJobCount;
}

void WorkerThread::run()
{
    concurrency::setThreadName(_name.c_str());
    workerThreadHandler = this;
    _backgroundJobs.reserve(512);

    try
    {
        const int64_t maxWait2ms = 64 << 15;
        int64_t pollInterval = 64;

        while (_running)
        {
            auto jobProcessed = processJobs();
            if (jobProcessed)
            {
                pollInterval = 64;
            }
            else
            {
                utils::Time::nanoSleep(pollInterval);
                pollInterval = std::min(maxWait2ms, pollInterval * 2);
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
            if (_backgroundJobCount == _backgroundJobs.size())
            {
                _backgroundJobs.push_back({false, job});
                ++_backgroundJobCount;
            }
            else
            {
                for (auto& slot : _backgroundJobs)
                {
                    if (!slot.job)
                    {
                        slot = {false, job};
                        ++_backgroundJobCount;
                        break;
                    }
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
    if (wt && wt->_yieldEnabled)
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
