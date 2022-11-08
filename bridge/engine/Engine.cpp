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

Engine::Engine(jobmanager::JobManager& backgroundJobQueue)
    : _messageListener(nullptr),
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

    const int64_t TICK_TOLERANCE = utils::Time::us * 500;
    const int64_t IDLE_MARGIN = utils::Time::us * 50;
    uint64_t timestamp = utils::Time::getAbsoluteTime();
    uint64_t statsPollTime = timestamp - utils::Time::sec * 2;
    while (_running)
    {
        const auto overShoot = -pacer.timeToNextTick(timestamp);
        if (overShoot >= TICK_TOLERANCE)
        {
            ++currentStatSample.timeSlipCount; // missed tick by 0.5ms
        }
        pacer.tick(timestamp);

        for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
        {
            assert(mixerEntry->_data);
            mixerEntry->_data->run(timestamp);
        }

        if (++_tickCounter % STATS_UPDATE_TICKS == 0)
        {
            updateStats(statsPollTime, currentStatSample, timestamp);
        }

        // process tasks and forward packets until next tick is near
        timestamp = utils::Time::getAbsoluteTime();
        int64_t toSleep = pacer.timeToNextTick(timestamp);
        int64_t nextForwardCycle = toSleep - utils::Time::ms;
        while (toSleep > IDLE_MARGIN)
        {
            // forward packets every ms
            if (toSleep < nextForwardCycle && toSleep >= static_cast<int64_t>(utils::Time::ms))
            {
                for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
                {
                    assert(mixerEntry->_data);
                    mixerEntry->_data->forwardPackets(timestamp);
                }
                nextForwardCycle -= utils::Time::ms;
            }

            const auto pendingTasks = processTasks(128);
            timestamp = utils::Time::getAbsoluteTime();
            toSleep = pacer.timeToNextTick(timestamp);
            if (!pendingTasks && toSleep > 0)
            {
                utils::Time::nanoSleep(std::min(utils::checkedCast<uint64_t>(toSleep), utils::Time::us * 2000));
                timestamp = utils::Time::getAbsoluteTime();
                toSleep = pacer.timeToNextTick(timestamp);
            }
        }

        if (toSleep > 0)
        {
            utils::Time::nanoSleep(utils::checkedCast<uint64_t>(toSleep));
            timestamp = utils::Time::getAbsoluteTime();
        }
    }
}

/* @return true if there are pending tasks */
bool Engine::processTasks(uint32_t maxCount)
{
    for (uint32_t jobCount = 0; jobCount < maxCount; ++jobCount)
    {
        utils::Function job;
        if (_tasks.pop(job))
        {
            job();
        }
        else
        {
            return false;
        }
    }

    return !_tasks.empty();
}

void Engine::updateStats(uint64_t& statsPollTime, EngineStats::EngineStats& currentStatSample, const uint64_t timestamp)
{
    uint64_t pollTime = utils::Time::getAbsoluteTime();
    currentStatSample.activeMixers = EngineStats::MixerStats();
    for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
    {
        currentStatSample.activeMixers += mixerEntry->_data->gatherStats(timestamp);
    }

    currentStatSample.pollPeriodMs =
        static_cast<uint32_t>(std::max(uint64_t(1), (pollTime - statsPollTime) / uint64_t(1000000)));
    _stats.write(currentStatSample);
    statsPollTime = pollTime;
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
