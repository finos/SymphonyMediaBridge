#pragma once
#include "utils/ScopedIncrement.h"
#include "utils/Time.h"
#include <atomic>

namespace jobmanager
{

class Job
{
public:
    Job() {}

    virtual void run() = 0;
    virtual ~Job() = default;
};

/**
 * Job that counts itseft in a counter help by job's owner.
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
