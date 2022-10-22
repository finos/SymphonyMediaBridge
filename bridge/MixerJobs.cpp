#include "MixerJobs.h"
#include "Mixer.h"
#include "MixerManager.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMessageListener.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"

namespace bridge
{
EngineMixerRemoved::EngineMixerRemoved(EngineMessageListener& mixerManager, EngineMixer& mixer)
    : _mixerManager(mixerManager),
      _engineMixer(mixer),
      _step(Acquire)
{
}

bool EngineMixerRemoved::runStep()
{
    if (_step == Acquire)
    {
        _mixer = _mixerManager.onEngineMixerRemoved1(_engineMixer);
        if (!_mixer)
        {
            return false;
        }
        _step = Stopping;
    }

    if (_step == Stopping)
    {
        // TODO add timeout
        if (_mixer->hasPendingTransportJobs())
        {
            return true;
        }
        _mixerManager.onEngineMixerRemoved2(_mixer->getId());
        // must not touch engineMixer anymore
        _mixer.reset();
        return false;
    }

    return true;
}
} // namespace bridge
