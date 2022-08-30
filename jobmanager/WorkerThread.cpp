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
      _thread([this] { this->run(); })
{
}

void WorkerThread::stop()
{
    _running = false;
    _thread.join();
}

void WorkerThread::run()
{
    concurrency::setThreadName("Worker");
    workerThreadHandler = this;

    try
    {
        while (_running)
        {
            auto job = _jobManager.wait();
            if (!job)
            {
                continue;
            }

            job->run();

            _jobManager.freeJob(job);
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

bool WorkerThread::yield()
{
    WorkerThread* wt = workerThreadHandler;
    if (wt)
    {
        Job* job = wt->_jobManager.tryFetchNoWait();
        if (job)
        {
            job->run();
            wt->_jobManager.freeJob(job);
            return true;
        }
    }

    return false;
}

bool WorkerThread::isWorkerThread()
{
    return workerThreadHandler != nullptr;
}

} // namespace jobmanager
