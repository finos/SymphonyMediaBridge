#include "ThreadUtils.h"
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#else
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif
#include "logger/Logger.h"
namespace concurrency
{
bool setPriority(std::thread& thread, Priority priority)
{
    if (priority == Priority::Normal)
    {
        return true;
    }
    auto threadId = thread.native_handle();

#ifdef __APPLE__
    struct mach_timebase_info machTimeBase({});
    mach_timebase_info(&machTimeBase);

    thread_port_t threadPort = pthread_mach_thread_np(threadId);

    thread_extended_policy_data_t policy;
    policy.timeshare = 0; // Set to 1 for a non-fixed thread.
    auto result =
        thread_policy_set(threadPort, THREAD_EXTENDED_POLICY, (thread_policy_t)&policy, THREAD_EXTENDED_POLICY_COUNT);

    if (result != KERN_SUCCESS)
    {
        logger::warn("thread_policy_set() failure: ", "");
        return false;
    }

    // Set to relatively high priority.
    thread_precedence_policy_data_t precedence;
    precedence.importance = 63;
    result = thread_policy_set(threadPort,
        THREAD_PRECEDENCE_POLICY,
        (thread_policy_t)&precedence,
        THREAD_PRECEDENCE_POLICY_COUNT);

    if (result != KERN_SUCCESS)
    {
        logger::warn("thread_policy_set() failure: ", "");
        return false;
    }

    thread_time_constraint_policy timeConstraintPolicy({});
    timeConstraintPolicy.period = 10000000 * machTimeBase.denom / machTimeBase.numer;
    timeConstraintPolicy.computation = 7000000 * machTimeBase.denom / machTimeBase.numer;
    timeConstraintPolicy.constraint = 9000000 * machTimeBase.denom / machTimeBase.numer;
    timeConstraintPolicy.preemptible = FALSE;

    if (thread_policy_set(threadPort,
            THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&timeConstraintPolicy),
            THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
    {
        logger::error("Unable to set thread priority", "");
        return false;
    }
    return true;
#else
    sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;

    auto rc = pthread_setschedparam(threadId, SCHED_FIFO, &param);
    if (rc == 0)
    {
        return true;
    }
    if (rc == EPERM)
    {
        logger::warn(
            "Failed to set thread priority to real-time %d. Not permitted. Use setcap CAP_SYS_NICE <executable>",
            "",
            rc);
    }
    else
    {
        logger::warn(
            "Failed to set thread priority to real-time %d. Check that this executable has CAP_SYS_NICE with getcap",
            "",
            rc);
    }
    return false;

#endif
}

void setThreadName(const char* name)
{
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}
} // namespace concurrency