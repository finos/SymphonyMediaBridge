#include "MixerJobs.h"
#include "Mixer.h"
#include "MixerManager.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"

namespace bridge
{
FinalizeEngineMixerRemoval::FinalizeEngineMixerRemoval(MixerManager& mixerManager, std::shared_ptr<Mixer> mixer)
    : _mixerManager(mixerManager),
      _mixer(mixer)
{
}

bool FinalizeEngineMixerRemoval::runStep()
{
    // TODO add timeout
    if (_mixer->hasPendingTransportJobs())
    {
        return true;
    }

    _mixerManager.finalizeEngineMixerRemoval(_mixer->getId());
    // must not touch engineMixer anymore
    _mixer.reset();
    return false;
}
} // namespace bridge
