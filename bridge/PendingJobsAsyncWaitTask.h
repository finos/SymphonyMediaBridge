
#pragma once

#include "jobmanager/AsyncWaiter.h"
#include "logger/Logger.h"
#include "transport/Transport.h"
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace transport
{
class Transport;
}

namespace bridge
{

enum class AsyncWaitTaskLogPolicy
{
    DEB,
    DEB_INFO,
    DEB_WARN,
    DEB_ERROR,
    INFO,
    INFO_WARN,
    INFO_ERROR,

};

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
class PendingJobsAsyncWaitTask : public jobmanager::AsyncWaitTask
{
    using TThis = PendingJobsAsyncWaitTask<LogPolicy, T, Args...>;

public:
    using OnTaskEndHandler = std::function<void()>;

    template <class... UTypes>
    PendingJobsAsyncWaitTask(const char* loggableId,
        uint32_t _taskId,
        T&& pendingJobsMonitor,
        UTypes&&... memoryToHold);

    bool canComplete() final;
    void onComplete() final;
    void onTimeout() final;

private:
    template <AsyncWaitTaskLogPolicy Policy1, AsyncWaitTaskLogPolicy Policy2, AsyncWaitTaskLogPolicy... PolicyArgs>
    static bool isLogPolicyOneOf()
    {
        return LogPolicy == Policy1 || TThis::isLogPolicyOneOf<Policy2, PolicyArgs...>();
    }

    template <AsyncWaitTaskLogPolicy Policy1>
    static bool isLogPolicyOneOf()
    {
        return LogPolicy == Policy1;
    }

private:
    const char* _loggableId;
    uint32_t _taskId;
    T _pendingJobsMonitor;
    std::tuple<Args...> _memoryToHold;
};

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
template <class... UTypes>
PendingJobsAsyncWaitTask<LogPolicy, T, Args...>::PendingJobsAsyncWaitTask(const char* loggableId,
    uint32_t _taskId,
    T&& pendingJobsMonitor,
    UTypes&&... memoryToHold)
    : _loggableId(loggableId),
      _taskId(_taskId),
      _pendingJobsMonitor(std::move(pendingJobsMonitor)),
      _memoryToHold(std::forward<UTypes>(memoryToHold)...)
{
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
bool PendingJobsAsyncWaitTask<LogPolicy, T, Args...>::canComplete()
{
    return !_pendingJobsMonitor->hasPendingJobs();
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
void PendingJobsAsyncWaitTask<LogPolicy, T, Args...>::onComplete()
{
    if (TThis::isLogPolicyOneOf<AsyncWaitTaskLogPolicy::INFO,
            AsyncWaitTaskLogPolicy::INFO_WARN,
            AsyncWaitTaskLogPolicy::INFO_ERROR>())
    {
        logger::info("PendingJobsAsyncWaitTask %u has completed", _loggableId, _taskId);
    }
    else
    {
        logger::debug("PendingJobsAsyncWaitTask %u has completed", _loggableId, _taskId);
    }
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
void PendingJobsAsyncWaitTask<LogPolicy, T, Args...>::onTimeout()
{
    if (TThis::isLogPolicyOneOf<AsyncWaitTaskLogPolicy::DEB_ERROR, AsyncWaitTaskLogPolicy::INFO_ERROR>())
    {
        logger::error("PendingJobsAsyncWaitTask %u has timed out", _loggableId, _taskId);
    }
    else if (TThis::isLogPolicyOneOf<AsyncWaitTaskLogPolicy::DEB_WARN, AsyncWaitTaskLogPolicy::INFO_WARN>())
    {
        logger::warn("PendingJobsAsyncWaitTask %u has timed out", _loggableId, _taskId);
    }
    else if (TThis::isLogPolicyOneOf<AsyncWaitTaskLogPolicy::INFO, AsyncWaitTaskLogPolicy::DEB_INFO>())
    {
        logger::info("PendingJobsAsyncWaitTask %u has timed out", _loggableId, _taskId);
    }
    else
    {
        logger::debug("PendingJobsAsyncWaitTask %u has timed out", _loggableId, _taskId);
    }
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
PendingJobsAsyncWaitTask<LogPolicy, std::shared_ptr<T>, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    std::shared_ptr<T> pendingJobsMonitor,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<LogPolicy, std::shared_ptr<T>, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsMonitor),
        std::forward<Args>(memoryToHold)...);
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
PendingJobsAsyncWaitTask<LogPolicy, std::unique_ptr<T>, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    std::unique_ptr<T>&& pendingJobsMonitor,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<LogPolicy, std::shared_ptr<T>, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsMonitor),
        std::forward<Args>(memoryToHold)...);
}

template <AsyncWaitTaskLogPolicy LogPolicy, class T, class... Args>
PendingJobsAsyncWaitTask<LogPolicy, T*, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    T* pendingJobsMonitor,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<LogPolicy, T*, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsMonitor), // It expects a rvalue, so move the pointer which does not move the object
        std::forward<Args>(memoryToHold)...);
}

} // namespace bridge
