#pragma once

#include "bridge/engine/EngineMessage.h"

namespace bridge
{

class EngineMessageListener
{
public:
    virtual ~EngineMessageListener() = default;

    virtual void onMessage(EngineMessage::Message&& message) = 0;
};

} // namespace bridge
