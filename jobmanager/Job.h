#pragma once
#include "utils/ScopedIncrement.h"
#include <atomic>

namespace jobmanager
{

// Job that can be run multiple times until it returns false.
// Use this to implement long running jobs that can continue where it left off.
// Track the state inside your job to execute the next step when run again.
class MultiStepJob
{
public:
    MultiStepJob() = default;
    virtual ~MultiStepJob() = default;

    // return true if job needs to be run again
    virtual bool runStep() = 0;
};

// Job that can only run once
class Job : public MultiStepJob
{
public:
    Job() {}

    virtual void run() = 0;
    virtual ~Job() = default;

private:
    bool runStep() override
    {
        run();
        return false; // do not run again
    }
};

/**
 * Job that counts itself in a counter help by job's owner.
 * This allows the job owner know if there are in-flight jobs.
 */
class CountedJob : public Job
{
public:
    explicit CountedJob(std::atomic_uint32_t& jobsCounter) : _jobsCounterIncrement(jobsCounter) {}

private:
    utils::ScopedIncrement _jobsCounterIncrement;
};

} // namespace jobmanager
