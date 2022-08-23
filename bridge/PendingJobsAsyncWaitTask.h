
#pragma once

#include "jobmanager/AsyncWaiter.h"
#include "bridge/engine/UntypedEngineObject.h"
#include "logger/Logger.h"
#include <memory>
#include <functional>
#include <string>

namespace transport
{
class Transport;
}

namespace bridge
{

class PendingJobsAsyncWaitTask : public jobmanager::AsyncWaitTask
{
public:
    using OnTaskEndHandler = std::function<void()>;

    PendingJobsAsyncWaitTask(const logger::LoggableId& loggableId,
        const std::shared_ptr<const transport::Transport>& _transport,
        const std::shared_ptr<UntypedEngineObject>& streamToHold,
        const std::string& endpointId);

    bool checkCompletion() final;
    void onComplete() final;
    void onTimeout() final;

    void setOnTaskEndHandler(OnTaskEndHandler onTaskEndHandler) { _onTaskEndHandler = onTaskEndHandler; }

private:
    const logger::LoggableId* _loggableId;
    std::shared_ptr<const transport::Transport> _transport;
    std::shared_ptr<UntypedEngineObject> _streamToHold;
    std::string _endpointId;
    OnTaskEndHandler _onTaskEndHandler;
};

} // namespace bridge