#pragma once

#include "bridge/engine/EngineMessage.h"
#include <string>

namespace bridge
{
class Mixer;
class EngineMixer;

class EngineMessageListener
{
public:
    virtual ~EngineMessageListener() = default;

    virtual bool onMessage(EngineMessage::Message&& message) = 0;

    virtual std::shared_ptr<Mixer> onEngineMixerRemoved1(EngineMixer& mixer) = 0;
    virtual void onEngineMixerRemoved2(const std::string& mixerId) = 0;
};

} // namespace bridge
