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

    /**
     * @brief Checks whether current thread is a WorkerThread or not
     * @return true if current thread is a worker thread; otherwise, false.
     */
    static bool isWorkerThread();

    /**
     * @brief Causes the calling thread to yield execution to another job that is ready to run on
     * jobManager attached to current WorkerThread.
     * @return true if has executed to another job; otherwise, false.
     */
    static bool yield();

private:
    std::atomic<bool> _running;
    jobmanager::JobManager& _jobManager;

    void run();
    static void threadEntry(WorkerThread* instance);

    std::thread _thread; // must be last
};

} // namespace jobmanager
