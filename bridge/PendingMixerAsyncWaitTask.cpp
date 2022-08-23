#include "PendingMixerAsyncWaitTask.h"
#include "bridge/MixerManager.h"
#include "bridge/Mixer.h"
#include "bridge/engine/EngineMixer.h"
#include "logger/Logger.h"

using namespace bridge;

PendingMixerAsyncWaitTask::PendingMixerAsyncWaitTask(MixerManager& mixerManager, std::unique_ptr<Mixer>&& mixer, std::unique_ptr<EngineMixer>&& engineMixer)
    : _mixerManager(&mixerManager),
      _mixer(std::move(mixer)),
      _engineMixer(std::move(engineMixer)),
      _mixerId(_mixer->getId())
{
}

bool PendingMixerAsyncWaitTask::checkCompletion()
{
    return !_mixer->hasPendingJobs();
}

void PendingMixerAsyncWaitTask::onComplete()
{
    logger::debug("Mixer %s has completed pending jobs. Removing EngineMixer %s", "PendingMixerAsyncWaitTask",
            _mixer->getLoggableId().c_str(),
            _engineMixer->getLoggableId().c_str());

    _engineMixer.reset();
    _mixer.reset();

    _mixerManager->onPendingMixerAsyncTaskEnd(_mixerId);
}

void PendingMixerAsyncWaitTask::onTimeout()
{
    logger::warn("Mixer %s has not completed pending jobs in time. Removing EngineMixer %s", "PendingMixerAsyncWaitTask",
            _mixer->getId().c_str(),
            _engineMixer->getLoggableId().c_str());

    _engineMixer.reset();
    _mixer.reset();

    _mixerManager->onPendingMixerAsyncTaskEnd(_mixerId);
}