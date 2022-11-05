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
    Engine(const config::Config& config, jobmanager::JobManager& backgroundJobQueue);

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

    const config::Config& _config;
    MixerManagerAsync* _messageListener;
    std::atomic<bool> _running;

    memory::List<EngineMixer*, maxMixers> _mixers;

    concurrency::MpmcPublish<EngineStats::EngineStats, 4> _stats;
    uint32_t _tickCounter;

    concurrency::MpmcQueue<utils::Function> _tasks;

    std::thread _thread; // must be last member

    /* @return true if max tasks number were reached */

public: // async methods called via post
    void addMixer(EngineMixer* engineMixer);
    void removeMixer(EngineMixer* engineMixer);

    void removeRecordingStream(EngineMixer* mixer, EngineRecordingStream* recordingStream);

    void stopRecording(EngineMixer* engineMixer,
        EngineRecordingStream* recordingStream,
        RecordingDescription* recordingDesc);
};

} // namespace bridge
