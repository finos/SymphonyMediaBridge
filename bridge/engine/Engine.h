#pragma once

#include "bridge/engine/IEngine.h"
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

class Engine : public IEngine
{
public:
    Engine(jobmanager::JobManager& backgroundJobQueue);
    Engine(jobmanager::JobManager& backgroundJobQueue, std::thread&& externalThread);

    void setMessageListener(MixerManagerAsync* messageListener) override;
    void stop() override;
    void run() override;

    bool post(utils::Function&& task) override { return _tasks.push(std::move(task)); }

    concurrency::SynchronizationContext getSynchronizationContext() override
    {
        return concurrency::SynchronizationContext(_tasks);
    }

    EngineStats::EngineStats getStats() override;

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

public:
    bool asyncAddMixer(EngineMixer* engineMixer) override;
    bool asyncRemoveMixer(EngineMixer* engineMixer) override;

private:
    void addMixer(EngineMixer* engineMixer);
    void removeMixer(EngineMixer* engineMixer);
};

} // namespace bridge
