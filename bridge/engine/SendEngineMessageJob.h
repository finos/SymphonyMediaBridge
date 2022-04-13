#pragma once

#include "bridge/engine/EngineMessage.h"
#include "bridge/engine/EngineMessageListener.h"
#include "jobmanager/Job.h"
#include "transport/Transport.h"

namespace bridge
{

class SendEngineMessageJob : public jobmanager::CountedJob
{
public:
    SendEngineMessageJob(transport::Transport& transport,
        EngineMessageListener& messageListener,
        EngineMessage::Message&& message)
        : CountedJob(transport.getJobCounter()),
          _messageListener(messageListener),
          _message(std::move(message))
    {
    }

    void run() override { _messageListener.onMessage(std::move(_message)); }

private:
    EngineMessageListener& _messageListener;
    EngineMessage::Message _message;
};

} // namespace bridge
