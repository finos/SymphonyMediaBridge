#include "PendingJobsAsyncWaitTask.h"
#include "transport/Transport.h"
#include "logger/Logger.h"

using namespace bridge;

PendingJobsAsyncWaitTask::PendingJobsAsyncWaitTask(
    const logger::LoggableId& loggableId,
    const std::shared_ptr<const transport::Transport>& transport,
    const std::shared_ptr<UntypedEngineObject>& streamToHold,
    const std::string& endpointId)
    : _loggableId(&loggableId),
      _transport(transport),
      _streamToHold(streamToHold),
      _endpointId(endpointId)
{
    logger::info("Wait for pending jobs on transport for endpointId %s", _loggableId->c_str(), _endpointId.c_str());
}

bool PendingJobsAsyncWaitTask::checkCompletion()
{
    return !_transport->hasPendingJobs();
}

void PendingJobsAsyncWaitTask::onComplete()
{
    logger::info("Transport for endpointId %s has finished pending jobs", _loggableId->c_str(), _endpointId.c_str());

    if (_onTaskEndHandler)
    {
        _onTaskEndHandler();
    }
}

void PendingJobsAsyncWaitTask::onTimeout()
{
    logger::error("Transport for endpointId %s did not finish pending jobs in time. deletion anyway.",
        _loggableId->c_str(),
        _endpointId.c_str());

    if (_onTaskEndHandler)
    {
        _onTaskEndHandler();
    }
}