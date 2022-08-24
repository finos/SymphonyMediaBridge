#include "jobmanager/WorkerThread.h"
#include "concurrency/ThreadUtils.h"
#include "jobmanager/JobManager.h"

namespace
{

thread_local bool threadLocalIsWorkerThread = false;

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
    threadLocalIsWorkerThread = true;

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
}

bool WorkerThread::isWorkerThread()
{
    return threadLocalIsWorkerThread;
}


} // namespace jobmanager
