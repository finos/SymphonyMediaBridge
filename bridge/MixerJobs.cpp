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
#include "logger/Logger.h"
#include "transport/RtcTransport.h"

namespace
{
constexpr const uint64_t TIMEOUT = 15 * utils::Time::sec;

} // namespace

using namespace bridge;
FinalizeEngineMixerRemoval::FinalizeEngineMixerRemoval(MixerManager& mixerManager, std::shared_ptr<Mixer> mixer)
    : jobmanager::MultiStepWithTimeoutJob(TIMEOUT),
      _mixerManager(mixerManager),
      _mixer(mixer)
{
}

void FinalizeEngineMixerRemoval::onTimeout()
{
    const bool hasPendingJobs = _mixer->hasPendingTransportJobs();
    logger::error("FinalizeEngineMixerRemoval has timedout. Mixer will be destroy anyway. Has pending jobs %s",
        _mixer->getLoggableId().c_str(),
        hasPendingJobs ? "t" : "f");

    _mixerManager.finalizeEngineMixerRemoval(_mixer->getId());
    _mixer.reset();
}

bool FinalizeEngineMixerRemoval::runTick()
{
    if (_mixer->hasPendingTransportJobs())
    {
        return true;
    }

    _mixerManager.finalizeEngineMixerRemoval(_mixer->getId());
    // must not touch engineMixer anymore
    _mixer.reset();
    return false;
}

FinalizeTransportJob::FinalizeTransportJob(const std::shared_ptr<transport::RtcTransport>& transport)
    : jobmanager::MultiStepWithTimeoutJob(TIMEOUT),
      _transport(transport)
{
}

void FinalizeTransportJob::onTimeout()
{
    const bool hasPendingJobs = _transport->hasPendingJobs();
    logger::error("FinalizeTransportJob has timedout. Transport will be destroy anyway. Has pending jobs %s",
        _transport->getLoggableId().c_str(),
        hasPendingJobs ? "t" : "f");

    _transport.reset();
}

bool FinalizeTransportJob::runTick()
{
    if (_transport->hasPendingJobs())
    {
        return true;
    }

    _transport.reset();
    return false;
}
