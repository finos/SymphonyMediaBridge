#pragma once

#include "bridge/Stats.h"
#include "bridge/engine/EngineMessageListener.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineStats.h"
#include "concurrency/MpmcQueue.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bridge
{
class Engine;
}

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
} // namespace jobmanager

namespace config
{
class Config;
}

namespace bridge
{

class Mixer;

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
        memory::PacketPoolAllocator& sendAllocator,
        memory::AudioPacketPoolAllocator& audioAllocator);

    ~MixerManager();

    bridge::Mixer* create();
    bridge::Mixer* create(uint32_t lastN);
    void remove(const std::string& id);
    std::vector<std::string> getMixerIds();
    std::unique_lock<std::mutex> getMixer(const std::string& id, Mixer*& outMixer);

    void stop();
    void run();
    void onMessage(EngineMessage::Message&& message) override;
    Stats::MixerManagerStats getStats();

private:
    struct MixerStats
    {
        uint32_t conferences = 0;
        uint32_t audioStreams = 0;
        uint32_t videoStreams = 0;
        uint32_t dataStreams = 0;
        uint32_t ticksSinceLastUpdate = 0;
        uint32_t largestConference = 0;
        EngineStats::EngineStats engine;
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
    memory::AudioPacketPoolAllocator& _audioAllocator;

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
    void engineMessageSctp(EngineMessage::Message&& message);
    void engineRecordingStopped(const EngineMessage::Message& message);
    void engineMessageAllocateRecordingRtpPacketCache(const EngineMessage::Message& message);
    void engineMessageFreeRecordingRtpPacketCache(const EngineMessage::Message& message);
    void engineMessageRemoveRecordingTransport(const EngineMessage::Message& message);
    void engineBarbellRemoved(const EngineMessage::EngineBarbellMessage& message);
    void engineBarbellIdle(const EngineMessage::EngineBarbellMessage& message);

    void updateStats();
};

} // namespace bridge
