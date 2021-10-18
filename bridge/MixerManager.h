#pragma once

#include "bridge/Stats.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineMessageListener.h"
#include "concurrency/MpmcQueue.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace utils
{
class IdGenerator;
class SsrcGenerator;
} // namespace utils

namespace transport
{
class TransportFactory;
} // namespace transport

namespace jobmanager
{
class JobManager;
class WorkerThread;
} // namespace jobmanager

namespace config
{
class Config;
}

namespace bridge
{

class Mixer;
class EngineMixer;

class MixerManager : public EngineMessageListener
{
public:
    MixerManager(utils::IdGenerator& idGenerator,
        utils::SsrcGenerator& ssrcGenerator,
        jobmanager::JobManager& jobManager,
        transport::TransportFactory& transportFactory,
        bridge::Engine& engine,
        const config::Config& config,
        memory::PacketPoolAllocator& mainAllocator,
        memory::PacketPoolAllocator& sendAllocator);

    ~MixerManager();

    bridge::Mixer* create();
    void remove(const std::string& id);
    std::vector<std::string> getMixerIds();
    std::unique_lock<std::mutex> getMixer(const std::string& id, Mixer*& outMixer);

    void stop();
    void run();
    void onMessage(const EngineMessage::Message& message) override;
    Stats::MixerManagerStats getStats();

private:
    struct MixerStats
    {
        MixerStats() : _conferences(0), _audioStreams(0), _videoStreams(0), _dataStreams(0), _ticksSinceLastUpdate(0) {}

        uint32_t _conferences;
        uint32_t _audioStreams;
        uint32_t _videoStreams;
        uint32_t _dataStreams;
        uint32_t _ticksSinceLastUpdate;
        EngineStats::EngineStats _engine;
    };

    utils::IdGenerator& _idGenerator;
    utils::SsrcGenerator& _ssrcGenerator;
    jobmanager::JobManager& _jobManager;
    transport::TransportFactory& _transportFactory;
    Engine& _engine;
    const config::Config& _config;

    std::unordered_map<std::string, std::unique_ptr<Mixer>> _mixers;
    std::unordered_map<std::string, std::unique_ptr<EngineMixer>> _engineMixers;
    std::unordered_map<std::string, std::unordered_map<uint32_t, std::unique_ptr<EngineMixer::AudioBuffer>>>
        _audioBuffers;

    std::atomic<bool> _threadRunning;
    std::unique_ptr<std::thread> _managerThread;
    concurrency::MpmcQueue<EngineMessage::Message> _engineMessages;
    std::mutex _configurationLock;

    MixerStats _stats;
    Stats::SystemStatsCollector _systemStatCollector;
    memory::PacketPoolAllocator& _mainAllocator;
    memory::PacketPoolAllocator& _sendAllocator;

    void engineMessageMixerRemoved(const EngineMessage::Message& message);
    void engineMessageAllocateAudioBuffer(const EngineMessage::Message& message);
    void engineMessageAudioStreamRemoved(const EngineMessage::Message& message);
    void engineMessageVideoStreamRemoved(const EngineMessage::Message& message);
    void engineMessageRecordingStreamRemoved(const EngineMessage::Message& message);
    void engineMessageDataStreamRemoved(const EngineMessage::Message& message);
    void engineMessageMixerTimedOut(const EngineMessage::Message& message);
    void engineMessageInboundSsrcRemoved(const EngineMessage::Message& message);
    void engineMessageAllocateVideoPacketCache(const EngineMessage::Message& message);
    void engineMessageFreeVideoPacketCache(const EngineMessage::Message& message);
    void engineMessageSctp(const EngineMessage::Message& message);
    void engineRecordingStopped(const EngineMessage::Message& message);
    void engineMessageAllocateRecordingRtpPacketCache(const EngineMessage::Message& message);
    void engineMessageFreeRecordingRtpPacketCache(const EngineMessage::Message& message);
    void engineMessageRemoveRecordingTransport(const EngineMessage::Message& message);

    void updateStats();
};

} // namespace bridge
