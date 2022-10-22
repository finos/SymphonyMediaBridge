#pragma once
#include "jobmanager/Job.h"
#include <memory>

namespace bridge
{
class EngineMessageListener;
class EngineMixer;
class Mixer;

class EngineMixerRemoved : public jobmanager::MultiStepJob
{
public:
    EngineMixerRemoved(EngineMessageListener& mixerManager, EngineMixer& mixer);

    bool runStep() override;

private:
    EngineMessageListener& _mixerManager;
    EngineMixer& _engineMixer;
    std::shared_ptr<Mixer> _mixer;
    enum
    {
        Acquire = 1,
        Stopping
    } _step;
};

} // namespace bridge
