#pragma once

#include <thread>

namespace jobmanager
{
class JobManager;

class WorkerThread
{
public:
    explicit WorkerThread(jobmanager::JobManager& jobManager);

    void stop();

    static double getWaitTime(); // ms
    static double getWorkTime(); // ms

    static bool isWorkerThread();
    static bool yield();

private:
    std::atomic<bool> _running;
    jobmanager::JobManager& _jobManager;

    void run();
    static void threadEntry(WorkerThread* instance);

    std::thread _thread; // must be last
};

} // namespace jobmanager
