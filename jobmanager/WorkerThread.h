#pragma once

#include "jobmanager/Job.h"
#include <thread>
#include <vector>

namespace jobmanager
{
class JobManager;

class WorkerThread
{
public:
    explicit WorkerThread(jobmanager::JobManager& jobManager, const char* name = nullptr);
    ~WorkerThread();

    void stop();

    static double getWaitTime(); // ms
    static double getWorkTime(); // ms

    static bool isWorkerThread();

    // returns true if there were jobs to process
    static bool yield();

private:
    std::atomic<bool> _running;
    jobmanager::JobManager& _jobManager;

    void run();
    bool processJobs();
    static void threadEntry(WorkerThread* instance);
    uint32_t processBackgroundJobs();

    struct BackgroundJob
    {
        bool running = false;
        MultiStepJob* job = nullptr;
    };
    std::vector<BackgroundJob> _backgroundJobs;
    uint32_t _backgroundJobCount;

    std::string _name;
    std::thread _thread; // must be last
};

} // namespace jobmanager
