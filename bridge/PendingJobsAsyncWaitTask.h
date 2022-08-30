
#pragma once

#include "jobmanager/AsyncWaiter.h"
#include "logger/Logger.h"
#include "transport/Transport.h"
#include <functional>
#include <memory>
#include <string>

namespace transport
{
class Transport;
}

namespace bridge
{

template <typename StreamType>
class PendingJobsAsyncWaitTask : public jobmanager::AsyncWaitTask
{
public:
    using OnTaskEndHandler = std::function<void()>;

    PendingJobsAsyncWaitTask(const logger::LoggableId& loggableId,
        const std::shared_ptr<const transport::Transport>& _transport,
        const std::shared_ptr<StreamType>& streamToHold,
        const std::string& endpointId);

    bool checkCompletion() final;
    void onComplete() final;
    void onTimeout() final;

    void setOnTaskEndHandler(OnTaskEndHandler onTaskEndHandler) { _onTaskEndHandler = onTaskEndHandler; }

private:
    const logger::LoggableId* _loggableId;
    std::shared_ptr<const transport::Transport> _transport;
    std::shared_ptr<StreamType> _streamToHold;
    std::string _endpointId;
    OnTaskEndHandler _onTaskEndHandler;
};

template <typename StreamType>
PendingJobsAsyncWaitTask<StreamType>::PendingJobsAsyncWaitTask(const logger::LoggableId& loggableId,
    const std::shared_ptr<const transport::Transport>& transport,
    const std::shared_ptr<StreamType>& streamToHold,
    const std::string& endpointId)
    : _loggableId(&loggableId),
      _transport(transport),
      _streamToHold(streamToHold),
      _endpointId(endpointId)
{
    logger::info("Wait for pending jobs on transport for endpointId %s", _loggableId->c_str(), _endpointId.c_str());
}

template <typename StreamType>
bool PendingJobsAsyncWaitTask<StreamType>::checkCompletion()
{
    return !_transport->hasPendingJobs();
}

template <typename StreamType>
void PendingJobsAsyncWaitTask<StreamType>::onComplete()
{
    logger::info("Transport for endpointId %s has finished pending jobs", _loggableId->c_str(), _endpointId.c_str());

    if (_onTaskEndHandler)
    {
        _onTaskEndHandler();
    }
}

template <typename StreamType>
void PendingJobsAsyncWaitTask<StreamType>::onTimeout()
{
    logger::error("Transport for endpointId %s did not finish pending jobs in time. deletion anyway.",
        _loggableId->c_str(),
        _endpointId.c_str());

    if (_onTaskEndHandler)
    {
        _onTaskEndHandler();
    }
}

} // namespace bridge
