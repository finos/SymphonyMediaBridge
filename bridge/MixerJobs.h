#pragma once
#include "jobmanager/Job.h"
#include <memory>

namespace bridge
{
class Mixer;
class MixerManager;

class FinalizeEngineMixerRemoval : public jobmanager::MultiStepJob
{
public:
    FinalizeEngineMixerRemoval(MixerManager& mixerManager, std::shared_ptr<Mixer> mixer);

    bool runStep() override;

private:
    MixerManager& _mixerManager;
    std::shared_ptr<Mixer> _mixer;
};

} // namespace bridge
