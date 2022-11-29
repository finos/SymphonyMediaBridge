#pragma once
#include "utils/ScopedIncrement.h"
#include "utils/Time.h"
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

class MultiStepWithTimeoutJob : public MultiStepJob
{
public:
    MultiStepWithTimeoutJob(uint64_t timeout) : _timeLimit(utils::Time::getAbsoluteTime() + timeout) {}

    virtual void onTimeout() {} // Do nothing by default
    virtual bool runTick() = 0;

    bool runStep() final
    {
        const bool shouldRunAgain = runTick();
        if (shouldRunAgain)
        {
            if (utils::Time::diffGE(_timeLimit, utils::Time::getAbsoluteTime(), 0))
            {
                onTimeout();
                return false;
            }
        }

        return shouldRunAgain;
    }

protected:
    const uint64_t _timeLimit;
};

// Job that can only run once
class Job : public MultiStepJob
{
public:
    Job() {}

    virtual void run() = 0;
    virtual ~Job() = default;

private:
    bool runStep() final
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

/**
Generic callable. The only restriction is that needs to implement operator()().
This is has some advantages over give a utils::function.

We can use the direct type returned by utils::bind which creates one less level of indirection as the result is a final
type and does not need to invoke more virtual methods We can use lambda functions directly. The lambda function itself
does not created dynamic memory (dynamic memory can happens when we transform the lambda on std::function).
*/
template <class Callable>
class CallableJob final : public Job
{
public:
    explicit CallableJob(const Callable& callable) : _callable(callable) {}
    explicit CallableJob(Callable&& callable) : _callable(std::move(callable)) {}

    void run() final { _callable(); }

private:
    Callable _callable;
};

template <class Callable>
class CallableCountedJob : public CountedJob
{
public:
    explicit CallableCountedJob(std::atomic_uint32_t& jobsCounter, const Callable& callable)
        : CountedJob(jobsCounter),
          _callable(callable)
    {
    }
    explicit CallableCountedJob(std::atomic_uint32_t& jobsCounter, Callable&& callable)
        : CountedJob(jobsCounter),
          _callable(std::move(callable))
    {
    }

    void run() final { _callable(); }

private:
    Callable _callable;
};

} // namespace jobmanager
