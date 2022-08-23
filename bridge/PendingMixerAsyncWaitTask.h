
#pragma once

#include "jobmanager/AsyncWaiter.h"
#include <memory>

namespace transport
{
class Transport;
}

namespace bridge
{

class MixerManager;
class Mixer;
class EngineMixer;

class PendingMixerAsyncWaitTask : public jobmanager::AsyncWaitTask
{
public:
    PendingMixerAsyncWaitTask(MixerManager& mixerManager, std::unique_ptr<Mixer>&& mixer, std::unique_ptr<EngineMixer>&& engineMixer);

    bool checkCompletion() final;
    void onComplete() final;
    void onTimeout() final;


private:
    MixerManager* _mixerManager;
    std::unique_ptr<Mixer> _mixer;
    std::unique_ptr<EngineMixer> _engineMixer;
    std::string _mixerId;
};

} // namespace bridge