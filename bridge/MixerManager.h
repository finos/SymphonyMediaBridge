#pragma once

#include "bridge/CodecCapabilities.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/Stats.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineStats.h"
#include "concurrency/MpmcQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Pacer.h"
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

class MixerManager final : public MixerManagerAsync
{
public:
    MixerManager(utils::IdGenerator& idGenerator,
        utils::SsrcGenerator& ssrcGenerator,
        jobmanager::JobManager& rtJobManager,
        jobmanager::JobManager& backgroundJobQueue,
        transport::TransportFactory& transportFactory,
        bridge::Engine& engine,
        const config::Config& config,
        memory::PacketPoolAllocator& mainAllocator,
        memory::PacketPoolAllocator& sendAllocator,
        memory::AudioPacketPoolAllocator& audioAllocator);

    ~MixerManager();

    bridge::Mixer* create(bool useGlobalPort, VideoCodecSpec videoCodecs = VideoCodecSpec());
    bridge::Mixer* create(uint32_t lastN, bool useGlobalPort, VideoCodecSpec videoCodecs = VideoCodecSpec());
    void remove(const std::string& id);
    std::vector<std::string> getMixerIds();
    std::unique_lock<std::mutex> getMixer(const std::string& id, Mixer*& outMixer);

    void stop();
    void maintenance(uint64_t timestamp);

    Stats::MixerManagerStats getStats();

    Stats::AggregatedBarbellStats getBarbellStats();
    void finalizeEngineMixerRemoval(const std::string& mixerId);

private:
    struct MixerStats
    {
        uint32_t conferences = 0;
        uint32_t audioStreams = 0;
        uint32_t videoStreams = 0;
        uint32_t dataStreams = 0;
        uint64_t lastRefreshTimestamp = 0;
        uint32_t largestConference = 0;
        EngineStats::EngineStats engine;
    };

    utils::IdGenerator& _idGenerator;
    utils::SsrcGenerator& _ssrcGenerator;
    jobmanager::JobManager& _rtJobManager;
    jobmanager::JobManager& _backgroundJobQueue;
    transport::TransportFactory& _transportFactory;
    Engine& _engine;
    const config::Config& _config;

    std::unordered_map<std::string, std::shared_ptr<Mixer>> _mixers;

    std::atomic_bool _maintenanceRunning;
    std::atomic<bool> _running;
    utils::Pacer _statsRefreshPacer;
    std::mutex _configurationLock;

    MixerStats _stats;
    Stats::SystemStatsCollector _systemStatCollector;
    memory::PacketPoolAllocator& _mainAllocator;
    memory::PacketPoolAllocator& _sendAllocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;

    void updateStats();

    // Async interface
    bool post(utils::Function&& task) override { return _backgroundJobQueue.post(std::move(task)); }

    void audioStreamRemoved(EngineMixer& mixer, const EngineAudioStream& audioStream) override;
    void engineMixerRemoved(EngineMixer& mixer) override;
    void freeVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) override;
    void allocateVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) override;
    void allocateRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) override;
    void videoStreamRemoved(EngineMixer& engineMixer, const EngineVideoStream& videoStream) override;
    void sctpReceived(EngineMixer& mixer, memory::UniquePacket msgPacket, size_t endpointIdHash) override;
    void dataStreamRemoved(EngineMixer& mixer, const EngineDataStream& dataStream) override;
    void freeRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) override;
    void barbellRemoved(EngineMixer& mixer, const EngineBarbell& barbell) override;
    void recordingStreamRemoved(EngineMixer& mixer, const EngineRecordingStream& recordingStream) override;
    void removeRecordingTransport(EngineMixer& mixer, EndpointIdString streamId, size_t endpointIdHash) override;
    void inboundSsrcContextRemoved(EngineMixer& mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder) override;
    void mixerTimedOut(EngineMixer& mixer) override;
    void engineRecordingStopped(EngineMixer& mixer, const RecordingDescription& recordingDesc) override;
};

} // namespace bridge
