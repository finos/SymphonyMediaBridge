#pragma once

#include "bridge/engine/EngineStats.h"
#include "concurrency/SynchronizationContext.h"
#include "utils/Function.h"

namespace bridge
{

class MixerManagerAsync;
class EngineMixer;
struct RecordingDescription;

class IEngine
{
public:
    virtual ~IEngine() = default;

    virtual void setMessageListener(MixerManagerAsync* messageListener) = 0;
    virtual void stop() = 0;
    virtual void run() = 0;

    virtual bool post(utils::Function&& task) = 0;

    virtual concurrency::SynchronizationContext getSynchronizationContext() = 0;

    virtual EngineStats::EngineStats getStats() = 0;

    virtual bool asyncAddMixer(EngineMixer* engineMixer) = 0;
    virtual bool asyncRemoveMixer(EngineMixer* engineMixer) = 0;
};

} // namespace bridge
