
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

template <class T, class... Args>
class PendingJobsAsyncWaitTask : public jobmanager::AsyncWaitTask
{
public:
    using OnTaskEndHandler = std::function<void()>;

    template <class... UTypes>
    PendingJobsAsyncWaitTask(const char* loggableId,
        uint32_t _taskId,
        T&& pendingJobsChecker,
        UTypes&&... memoryToHold);

    bool canComplete() final;
    void onComplete() final;
    void onTimeout() final;

private:
    const char* _loggableId;
    uint32_t _taskId;
    T _pendingJobsChecker;
    std::tuple<Args...> _memoryToHold;
};

template <class T, class... Args>
template <class... UTypes>
PendingJobsAsyncWaitTask<T, Args...>::PendingJobsAsyncWaitTask(const char* loggableId,
    uint32_t _taskId,
    T&& pendingJobsChecker,
    UTypes&&... memoryToHold)
    : _loggableId(loggableId),
      _taskId(_taskId),
      _pendingJobsChecker(std::move(pendingJobsChecker)),
      _memoryToHold(std::forward<UTypes>(memoryToHold)...)
{
}

template <class T, class... Args>
bool PendingJobsAsyncWaitTask<T, Args...>::canComplete()
{
    return !_pendingJobsChecker->hasPendingJobs();
}

template <class T, class... Args>
void PendingJobsAsyncWaitTask<T, Args...>::onComplete()
{
    logger::info("PendingJobsAsyncWaitTask %u has completed", _loggableId, _taskId);
}

template <class T, class... Args>
void PendingJobsAsyncWaitTask<T, Args...>::onTimeout()
{
    logger::error("PendingJobsAsyncWaitTask %u has timed out", _loggableId, _taskId);
}

template <class T, class... Args>
PendingJobsAsyncWaitTask<std::shared_ptr<T>, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    std::shared_ptr<T> pendingJobsChecker,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<std::shared_ptr<T>, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsChecker),
        std::forward<Args>(memoryToHold)...);
}

template <class T, class... Args>
PendingJobsAsyncWaitTask<std::unique_ptr<T>, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    std::unique_ptr<T>&& pendingJobsChecker,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<std::shared_ptr<T>, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsChecker),
        std::forward<Args>(memoryToHold)...);
}

template <class T, class... Args>
PendingJobsAsyncWaitTask<T*, Args...> makePendingJobsAsyncTask(const char* loggableId,
    uint32_t _taskId,
    T* pendingJobsChecker,
    Args&&... memoryToHold)
{
    return PendingJobsAsyncWaitTask<T*, Args...>(loggableId,
        _taskId,
        std::move(pendingJobsChecker), // It expects a rvalue, so move the pointer which does not move the object
        std::forward<Args>(memoryToHold)...);
}

} // namespace bridge
