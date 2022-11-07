#pragma once

#include "bridge/engine/EngineStats.h"
#include "concurrency/MpmcPublish.h"
#include "concurrency/MpmcQueue.h"
#include "concurrency/SynchronizationContext.h"
#include "config/Config.h"
#include "memory/List.h"
#include "utils/Trackers.h"
#include <sys/types.h>
#include <thread>

namespace jobmanager
{
class JobManager;
}

namespace bridge
{

class EngineMixer;
class MixerManagerAsync;
struct EngineRecordingStream;
struct RecordingDescription;

class Engine
{
public:
    Engine(jobmanager::JobManager& backgroundJobQueue);

    void setMessageListener(MixerManagerAsync* messageListener);
    void stop();
    void run();

    bool post(utils::Function&& task) { return _tasks.push(std::move(task)); }

    concurrency::SynchronizationContext getSynchronizationContext()
    {
        return concurrency::SynchronizationContext(_tasks);
    }

    EngineStats::EngineStats getStats();

private:
    static const size_t maxMixers = 4096;
    static const uint32_t STATS_UPDATE_TICKS = 200;

    MixerManagerAsync* _messageListener;
    std::atomic<bool> _running;

    memory::List<EngineMixer*, maxMixers> _mixers;

    concurrency::MpmcPublish<EngineStats::EngineStats, 4> _stats;
    uint32_t _tickCounter;

    concurrency::MpmcQueue<utils::Function> _tasks;

    std::thread _thread; // must be last member

    bool processTasks(uint32_t maxCount);
    void updateStats(uint64_t& statsPollTime, EngineStats::EngineStats& currentStatSample, uint64_t timestamp);

public: // async methods called via post
    void addMixer(EngineMixer* engineMixer);
    void removeMixer(EngineMixer* engineMixer);

    void removeRecordingStream(EngineMixer* mixer, EngineRecordingStream* recordingStream);

    void stopRecording(EngineMixer* engineMixer,
        EngineRecordingStream* recordingStream,
        RecordingDescription* recordingDesc);
};

} // namespace bridge
