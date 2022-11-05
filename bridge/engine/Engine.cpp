#include "bridge/engine/Engine.h"
#include "bridge/MixerJobs.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include "utils/CheckedCast.h"
#include "utils/Pacer.h"
#include <cassert>

namespace
{

const auto intervalNs = 1000000UL * bridge::EngineMixer::iterationDurationMs;

}

namespace bridge
{

Engine::Engine(const config::Config& config, jobmanager::JobManager& backgroundJobQueue)
    : _config(config),
      _messageListener(nullptr),
      _running(true),
      _tickCounter(0),
      _tasks(1024),
      _thread([this] { this->run(); })
{
    if (concurrency::setPriority(_thread, concurrency::Priority::RealTime))
    {
        logger::info("Successfully set thread priority to realtime.", "Engine");
    }
}

void Engine::setMessageListener(MixerManagerAsync* messageListener)
{
    _messageListener = messageListener;
}

void Engine::stop()
{
    _running = false;
    _thread.join();
    logger::debug("Engine stopped", "Engine");
}

void Engine::run()
{
    logger::debug("Engine started", "Engine");
    concurrency::setThreadName("Engine");
    utils::Pacer pacer(intervalNs);
    EngineStats::EngineStats currentStatSample;

    uint64_t previousPollTime = utils::Time::getAbsoluteTime() - utils::Time::sec * 2;
    while (_running)
    {
        const auto engineIterationStartTimestamp = utils::Time::getAbsoluteTime();
        pacer.tick(engineIterationStartTimestamp);

        for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
        {
            assert(mixerEntry->_data);
            mixerEntry->_data->run(engineIterationStartTimestamp);
        }

        if (++_tickCounter % STATS_UPDATE_TICKS == 0)
        {
            uint64_t pollTime = utils::Time::getAbsoluteTime();
            currentStatSample.activeMixers = EngineStats::MixerStats();
            for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
            {
                currentStatSample.activeMixers += mixerEntry->_data->gatherStats(engineIterationStartTimestamp);
            }

            currentStatSample.pollPeriodMs =
                static_cast<uint32_t>(std::max(uint64_t(1), (pollTime - previousPollTime) / uint64_t(1000000)));
            _stats.write(currentStatSample);
            previousPollTime = pollTime;
        }

        int64_t toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());
        int64_t nextForwardCycle = toSleep - utils::Time::ms;
        while (toSleep > 0)
        {
            // forward packets every ms
            if (toSleep < nextForwardCycle && toSleep >= static_cast<int64_t>(utils::Time::ms))
            {
                const auto timestamp = utils::Time::getAbsoluteTime();
                for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
                {
                    assert(mixerEntry->_data);
                    mixerEntry->_data->forwardPackets(timestamp);
                }
                nextForwardCycle -= utils::Time::ms;
            }

            for (int jobCount = 0; jobCount < 16 && toSleep > static_cast<int64_t>(utils::Time::us * 100); ++jobCount)
            {
                utils::Function job;
                if (_tasks.pop(job))
                {
                    job();
                }
                else
                {
                    toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());
                    utils::Time::nanoSleep(std::min(utils::checkedCast<uint64_t>(toSleep), utils::Time::us * 250));
                    break;
                }
            }
            toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());
        }

        if (toSleep <= 0)
        {
            ++currentStatSample.timeSlipCount;
        }
    }
}

void Engine::addMixer(EngineMixer* engineMixer)
{
    logger::debug("Adding mixer %s", "Engine", engineMixer->getLoggableId().c_str());
    if (!_mixers.pushToTail(engineMixer))
    {
        logger::error("Unable to add EngineMixer %s to Engine", "Engine", engineMixer->getLoggableId().c_str());

        _messageListener->post(utils::bind(&MixerManagerAsync::engineMixerRemoved, _messageListener, engineMixer));

        return;
    }
}

void Engine::removeMixer(EngineMixer* engineMixer)
{
    logger::debug("Removing mixer %s", "Engine", engineMixer->getLoggableId().c_str());
    engineMixer->clear();
    if (!_mixers.remove(engineMixer))
    {
        logger::error("Unable to remove EngineMixer %s from Engine", "Engine", engineMixer->getLoggableId().c_str());
    }

    _messageListener->post(utils::bind(&MixerManagerAsync::engineMixerRemoved, _messageListener, engineMixer));
}

void Engine::removeRecordingStream(EngineMixer* mixer, EngineRecordingStream* recordingStream)
{
    logger::debug("Remove recordingStream, mixer %s", "Engine", mixer->getLoggableId().c_str());

    mixer->removeRecordingStream(recordingStream);

    _messageListener->post(
        utils::bind(&MixerManagerAsync::recordingStreamRemoved, _messageListener, mixer, recordingStream));
}

void Engine::stopRecording(EngineMixer* engineMixer,
    EngineRecordingStream* recordingStream,
    RecordingDescription* recordingDesc)
{
    engineMixer->recordingStop(recordingStream, recordingDesc);

    _messageListener->post(
        utils::bind(&MixerManagerAsync::engineRecordingStopped, _messageListener, engineMixer, recordingDesc));
}

EngineStats::EngineStats Engine::getStats()
{
    EngineStats::EngineStats stats;
    _stats.read(stats);
    return stats;
}

} // namespace bridge
