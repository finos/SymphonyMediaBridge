#pragma once

#include "jobmanager/JobManager.h"
#include <memory>
#include <thread>

namespace jobmanager
{

class WorkerThread
{
public:
    explicit WorkerThread(jobmanager::JobManager& jobManager);

    void stop();

    static double getWaitTime(); // ms
    static double getWorkTime(); // ms
private:
    std::atomic<bool> _running;
    jobmanager::JobManager& _jobManager;

    void run();
    static void threadEntry(WorkerThread* instance);

    std::thread _thread; // must be last
};

} // namespace jobmanager
