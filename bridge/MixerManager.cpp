#include "bridge/MixerManager.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/Mixer.h"
#include "bridge/MixerJobs.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/PacketCache.h"
#include "concurrency/ThreadUtils.h"
#include "config/Config.h"
#include "jobmanager/JobManager.h"
#include "logger/Logger.h"
#include "transport/TransportFactory.h"
#include "utils/IdGenerator.h"
#include "utils/Pacer.h"
#include "utils/SsrcGenerator.h"
#include "utils/StringBuilder.h"
#include "utils/Time.h"
#include "webrtc/DataChannel.h"
#include <vector>

namespace
{

const uint32_t maxLastN = 16;

} // namespace

namespace bridge
{

class MixerManagerMainJob : public jobmanager::MultiStepJob
{
public:
    MixerManagerMainJob(MixerManager& mixerManager,
        std::atomic_bool& running,
        utils::Pacer& statsRefreshPacer,
        std::atomic_bool& maintenanceRunning)
        : _mixerManager(mixerManager),
          _running(running),
          _statsPacer(statsRefreshPacer),
          _maintenanceRunning(maintenanceRunning)
    {
        _statsPacer.tick(utils::Time::getAbsoluteTime());
    }

    bool runStep() override
    {
        if (!_running)
        {
            return false;
        }

        auto timestamp = utils::Time::getAbsoluteTime();
        const auto toSleep = _statsPacer.timeToNextTick(timestamp);
        if (toSleep > 0)
        {
            return _running.load();
        }
        else
        {
            _mixerManager.maintenance(timestamp);
            _statsPacer.tick(timestamp);
        }

        return _running.load();
    }

    ~MixerManagerMainJob() { _maintenanceRunning = false; }

private:
    MixerManager& _mixerManager;
    std::atomic_bool& _running;
    utils::Pacer& _statsPacer;
    std::atomic_bool& _maintenanceRunning;
};

MixerManager::MixerManager(utils::IdGenerator& idGenerator,
    utils::SsrcGenerator& ssrcGenerator,
    jobmanager::JobManager& rtJobManager,
    jobmanager::JobManager& backgroundJobQueue,
    transport::TransportFactory& transportFactory,
    Engine& engine,
    const config::Config& config,
    memory::PacketPoolAllocator& mainAllocator,
    memory::PacketPoolAllocator& sendAllocator,
    memory::AudioPacketPoolAllocator& audioAllocator)
    : _idGenerator(idGenerator),
      _ssrcGenerator(ssrcGenerator),
      _rtJobManager(rtJobManager),
      _backgroundJobQueue(backgroundJobQueue),
      _transportFactory(transportFactory),
      _engine(engine),
      _config(config),
      _running(true),
      _statsRefreshPacer(500 * utils::Time::ms),
      _mainAllocator(mainAllocator),
      _sendAllocator(sendAllocator),
      _audioAllocator(audioAllocator)
{
    _mixers.reserve(512);
    _engine.setMessageListener(this);
    _maintenanceRunning = true;
    _backgroundJobQueue.addJob<MixerManagerMainJob>(*this, _running, _statsRefreshPacer, _maintenanceRunning);
}

MixerManager::~MixerManager()
{
    logger::debug("Deleted", "MixerManager");
}

Mixer* MixerManager::create(bool useGlobalPort, VideoCodecSpec videoCodecs)
{
    return create(_config.defaultLastN, useGlobalPort, videoCodecs);
}

Mixer* MixerManager::create(uint32_t lastN, bool useGlobalPort, VideoCodecSpec videoCodecs)
{
    lastN = std::min(lastN, maxLastN);
    logger::info("Create mixer, last-n %u", "MixerManager", lastN);

    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto id = std::to_string(_idGenerator.next());
    const auto localVideoSsrc = _ssrcGenerator.next();

    std::vector<uint32_t> audioSsrcs;
    std::vector<api::SimulcastGroup> videoSsrcs;
    std::vector<api::SsrcPair> videoPinSsrcs;

    for (uint32_t i = 0; i < _config.audio.lastN + _config.audio.lastNextra; ++i)
    {
        audioSsrcs.push_back(_ssrcGenerator.next());
    }

    videoSsrcs.reserve(lastN + 3);
    // screen share / slides
    {
        api::SsrcPair a[1] = {{_ssrcGenerator.next(), _ssrcGenerator.next()}};
        videoSsrcs.push_back(api::SimulcastGroup(a));
    }
    // Last-n + extra
    for (uint32_t i = 0; i < lastN + 2; ++i)
    {
        api::SsrcPair a[3] = {{_ssrcGenerator.next(), _ssrcGenerator.next()},
            {_ssrcGenerator.next(), _ssrcGenerator.next()},
            {_ssrcGenerator.next(), _ssrcGenerator.next()}};
        videoSsrcs.push_back(api::SimulcastGroup(a));
    }

    for (uint32_t i = 0; i < 4; ++i)
    {
        videoPinSsrcs.push_back({_ssrcGenerator.next(), _ssrcGenerator.next()});
    }

    auto engineMixer = std::make_unique<EngineMixer>(id,
        _rtJobManager,
        _engine.getSynchronizationContext(),
        _backgroundJobQueue,
        *this,
        localVideoSsrc,
        _config,
        _sendAllocator,
        _audioAllocator,
        _mainAllocator,
        audioSsrcs,
        videoSsrcs,
        lastN);
    if (!engineMixer)
    {
        logger::error("Failed to create engineMixer", "MixerManager");
        return nullptr;
    }

    auto mixerEmplaceResult = _mixers.emplace(id,
        std::make_shared<Mixer>(id,
            engineMixer->getLoggableId().getInstanceId(),
            _transportFactory,
            _backgroundJobQueue,
            std::move(engineMixer),
            _idGenerator,
            _ssrcGenerator,
            _config,
            audioSsrcs,
            videoSsrcs,
            videoPinSsrcs,
            useGlobalPort,
            videoCodecs));
    if (!mixerEmplaceResult.second)
    {
        logger::error("Failed to create mixer", "MixerManager");
        return nullptr;
    }

    utils::StringBuilder<1024> b;
    b.append("local ").append(localVideoSsrc);
    b.append(" slides ").append(videoSsrcs[0][0].main);
    b.append(" pins");
    for (const auto& pinSsrc : videoPinSsrcs)
    {
        b.append(" ");
        b.append(pinSsrc.main);
    }

    logger::info("Mixer-%zu id=%s, %s",
        "MixerManager",
        mixerEmplaceResult.first->second->getLoggableId().getInstanceId(),
        id.c_str(),
        b.build().c_str());

    _engine.asyncAddMixer(mixerEmplaceResult.first->second->getEngineMixer());
    return mixerEmplaceResult.first->second.get();
}

void MixerManager::remove(const std::string& id)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto findResult = _mixers.find(id);
    if (findResult == _mixers.cend() || findResult->second->isMarkedForDeletion())
    {
        return;
    }

    findResult->second->markForDeletion();
    _engine.asyncRemoveMixer(findResult->second->getEngineMixer());
}

std::vector<std::string> MixerManager::getMixerIds()
{
    std::vector<std::string> result;

    std::lock_guard<std::mutex> locker(_configurationLock);
    for (const auto& mixerPair : _mixers)
    {
        result.emplace_back(mixerPair.first);
    }

    return result;
}

std::unique_lock<std::mutex> MixerManager::getMixer(const std::string& id, Mixer*& outMixer)
{
    std::unique_lock<std::mutex> locker(_configurationLock);

    auto iterator = _mixers.find(id);
    if (iterator != _mixers.cend() && !iterator->second->isMarkedForDeletion())
    {
        outMixer = iterator->second.get();
        return locker;
    }

    outMixer = nullptr;
    return locker;
}

void MixerManager::stop()
{
    if (!_running)
    {
        return;
    }

    logger::info("stopping", "MixerManager");
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        for (auto it = _mixers.begin(); it != _mixers.end(); ++it)
        {
            if (!it->second->isMarkedForDeletion())
            {
                it->second->markForDeletion();
                _engine.asyncRemoveMixer(it->second->getEngineMixer());
            }
        }
    }

    for (;; usleep(10000))
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        if (_mixers.empty())
            break;
    }

    _running = false; // signal to pending jobs we are not running anymore

    std::atomic_bool jobsDone(false);
    _backgroundJobQueue.post([&jobsDone]() { jobsDone = true; });
    for (;; usleep(10000))
    {
        if (jobsDone && !_maintenanceRunning.load())
        {
            break;
        }
    }

    logger::info("MixerManager thread stopped", "MixerManager");
}

void MixerManager::maintenance(uint64_t timestamp)
{
    try
    {
        if (!_running)
        {
            return;
        }
        _transportFactory.maintenance(timestamp);
        updateStats();
    }
    catch (std::exception e)
    {
        logger::error("Exception in MixerManager run: %s", "MixerManager", e.what());
    }
    catch (...)
    {
        logger::error("Unknown exception in MixerManager run", "MixerManager");
    }
}

void MixerManager::engineMixerRemoved(EngineMixer& engineMixer)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    std::string mixerId(engineMixer.getId()); // copy id string to have it after EngineMixer is deleted

    auto findResult = _mixers.find(mixerId);
    if (findResult != _mixers.end())
    {
        logger::info("Finalizing EngineMixer %s", "MixerManager", mixerId.c_str());
        auto mixer = findResult->second;
        _mixers.erase(mixerId);
        mixer->stopTransports(); // this will stop new packets from coming in
        _backgroundJobQueue.addJob<bridge::FinalizeEngineMixerRemoval>(*this, mixer);
    }
    else
    {
        logger::error("EngineMixer %s not found", "MixerManager", mixerId.c_str());
    }
}

void MixerManager::finalizeEngineMixerRemoval(const std::string& mixerId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    if (_mixers.empty())
    {
        _mainAllocator.logAllocatedElements();
        _sendAllocator.logAllocatedElements();
        _audioAllocator.logAllocatedElements();
    }

    logger::info("Mixer %s has been finalized", "MixerManager", mixerId.c_str());
}

void MixerManager::audioStreamRemoved(EngineMixer& mixer, const EngineAudioStream& audioStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::info("Removing audioStream endpointId %s from mixer %s",
        "MixerManager",
        audioStream.endpointId.c_str(),
        mixer.getLoggableId().c_str());

    const auto mixerIter = _mixers.find(mixer.getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            mixer.getLoggableId().c_str(),
            mixer.getId().c_str());
        return;
    }

    mixerIter->second->engineAudioStreamRemoved(audioStream);
}

void MixerManager::videoStreamRemoved(EngineMixer& engineMixer, const EngineVideoStream& videoStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::info("Removing videoStream endpointId %s from mixer %s",
        "MixerManager",
        videoStream.endpointId.c_str(),
        engineMixer.getLoggableId().c_str());

    const auto mixerIter = _mixers.find(engineMixer.getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            engineMixer.getLoggableId().c_str(),
            engineMixer.getId().c_str());
        return;
    }

    mixerIter->second->engineVideoStreamRemoved(videoStream);
}

void MixerManager::recordingStreamRemoved(EngineMixer& mixer, const EngineRecordingStream& recordingStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::info("Removing recordingStream  %s from mixer %s",
        "MixerManager",
        recordingStream.id.c_str(),
        mixer.getLoggableId().c_str());

    const auto mixerIter = _mixers.find(mixer.getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            mixer.getLoggableId().c_str(),
            mixer.getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingStreamRemoved(recordingStream);
}

void MixerManager::dataStreamRemoved(EngineMixer& mixer, const EngineDataStream& dataStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::info("Removing dataStream endpointId %s from mixer %s",
        "MixerManager",
        dataStream.endpointId.c_str(),
        mixer.getLoggableId().c_str());

    const auto mixerIter = _mixers.find(mixer.getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            mixer.getLoggableId().c_str(),
            mixer.getId().c_str());
        return;
    }

    mixerIter->second->engineDataStreamRemoved(dataStream);
}

void MixerManager::mixerTimedOut(EngineMixer& mixer)
{
    logger::info("Mixer %s inactivity time out. Deleting it.", "MixerManager", mixer.getLoggableId().c_str());
    remove(mixer.getId());
}

void MixerManager::inboundSsrcContextRemoved(EngineMixer& mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder)
{
    logger::info("Mixer %s removed ssrc %u", "MixerManager", mixer.getLoggableId().c_str(), ssrc);
    delete opusDecoder;
}

void MixerManager::allocateVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto mixerItr = _mixers.find(mixer.getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateVideoPacketCache(ssrc, endpointIdHash);
}

void MixerManager::freeVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto mixerItr = _mixers.find(mixer.getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeVideoPacketCache(ssrc, endpointIdHash);
}

void MixerManager::sctpReceived(EngineMixer& mixer, memory::UniquePacket msgPacket, size_t endpointIdHash)
{
    auto& sctpHeader = webrtc::streamMessageHeader(*msgPacket);

    if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
    {
        // create command with this packet to send the binary data -> engine -> WebRtcDataStream belonging to this
        // transport
        mixer.asyncHandleSctpControl(endpointIdHash, msgPacket);
        return; // do not free packet as we passed it on
    }
    else if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_STRING)
    {
        std::string body(reinterpret_cast<const char*>(sctpHeader.data()), msgPacket->getLength() - sizeof(sctpHeader));
        try
        {
            auto json = utils::SimpleJson::create(reinterpret_cast<const char*>(sctpHeader.data()),
                msgPacket->getLength() - sizeof(sctpHeader));

            if (api::DataChannelMessageParser::isPinnedEndpointsChanged(json))
            {
                logger::debug("received pin msg %s", "MixerManager", body.c_str());
                auto mixerIt = _mixers.find(mixer.getId());
                if (mixerIt != _mixers.end())
                {
                    const auto pinnedEndpoints = api::DataChannelMessageParser::getPinnedEndpoint(json);
                    if (pinnedEndpoints.isNone())
                    {
                        mixerIt->second->unpinEndpoint(endpointIdHash);
                    }
                    else
                    {
                        auto endpoints = pinnedEndpoints.getArray();
                        char endpointId[45];
                        endpoints.front().getString(endpointId);
                        mixerIt->second->pinEndpoint(endpointIdHash, endpointId);
                    }
                }
            }
            else if (api::DataChannelMessageParser::isEndpointMessage(json))
            {
                auto mixerIt = _mixers.find(mixer.getId());
                if (mixerIt == _mixers.end())
                {
                    return;
                }

                const auto toJson = api::DataChannelMessageParser::getEndpointMessageTo(json);
                const auto payloadJson = api::DataChannelMessageParser::getEndpointMessagePayload(json);
                if (payloadJson.isNone())
                {
                    logger::debug("endpoint message lacks content", "MixerManager");
                    return;
                }

                auto toEndpoint = toJson.getString();
                mixerIt->second->sendEndpointMessage(toEndpoint, endpointIdHash, payloadJson);
            }
            else
            {
                logger::debug("no handler for data channel message.", "MixerManager");
            }
        }
        catch (nlohmann::detail::parse_error e)
        {
            logger::error("Json parse error of DataChannel message. %s", "MixerManager", e.what());
        }
        catch (std::exception e)
        {
            logger::error("Exception while parsing DataChannel message. %s", "MixerManager", e.what());
        }
        catch (...)
        {
            logger::error("Unknown exception while parsing DataChannel message.", "MixerManager");
        }
    }
    else
    {
        logger::debug("received unexpected DataChannel payload protocol, %u, len %zu",
            "MixerManager",
            sctpHeader.payloadProtocol,
            msgPacket->getLength());
    }
}

void MixerManager::engineRecordingStopped(EngineMixer& mixer, const RecordingDescription& recordingDesc)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::info("Stopping recording %s from mixer %s",
        "MixerManager",
        recordingDesc.recordingId.c_str(),
        mixer.getLoggableId().c_str());

    const auto mixerIter = _mixers.find(mixer.getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            mixer.getLoggableId().c_str(),
            mixer.getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingDescStopped(recordingDesc);
}

void MixerManager::allocateRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto mixerItr = _mixers.find(mixer.getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateRecordingRtpPacketCache(ssrc, endpointIdHash);
}

void MixerManager::freeRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto mixerItr = _mixers.find(mixer.getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeRecordingRtpPacketCache(ssrc, endpointIdHash);
}

void MixerManager::removeRecordingTransport(EngineMixer& mixer, EndpointIdString streamId, size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto mixerItr = _mixers.find(mixer.getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->removeRecordingTransport(streamId.c_str(), endpointIdHash);
}

void MixerManager::barbellRemoved(EngineMixer& mixer, const EngineBarbell& barbell)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto mixerIt = _mixers.find(mixer.getId());
    if (mixerIt != _mixers.end())
    {
        mixerIt->second->engineBarbellRemoved(barbell);
    }
}

// This method may block up to 1s to collect the statistics
Stats::MixerManagerStats MixerManager::getStats()
{
    Stats::MixerManagerStats result;
    auto systemStats = _systemStatCollector.collect(_config.port, _config.ice.tcp.port);

    {
        std::lock_guard<std::mutex> locker(_configurationLock);

        result.conferences = _stats.conferences;
        result.videoStreams = _stats.videoStreams;
        result.audioStreams = _stats.audioStreams;
        result.dataStreams = _stats.dataStreams;
        result.engineStats = _stats.engine;
        result.systemStats = systemStats;
        result.largestConference = _stats.largestConference;
    }

    EndpointMetrics udpMetrics = _transportFactory.getSharedUdpEndpointsMetrics();

    result.jobQueueLength = _rtJobManager.getCount();
    result.receivePoolSize = _mainAllocator.size();
    result.sendPoolSize = _sendAllocator.size();
    result.udpSharedEndpointsSendQueue = udpMetrics.sendQueue;
    result.udpSharedEndpointsReceiveKbps = static_cast<uint32_t>(udpMetrics.receiveKbps);
    result.udpSharedEndpointsSendKbps = static_cast<uint32_t>(udpMetrics.sendKbps);
    result.udpSharedEndpointsSendDrops = udpMetrics.sendQueueDrops;

    return result;
}

void MixerManager::updateStats()
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    _stats.conferences = _mixers.size();
    _stats.videoStreams = 0;
    _stats.audioStreams = 0;
    _stats.dataStreams = 0;
    _stats.largestConference = 0;

    for (const auto& mixer : _mixers)
    {
        const auto stats = mixer.second->getStats();
        _stats.videoStreams += stats.videoStreams;
        _stats.audioStreams += stats.audioStreams;
        _stats.dataStreams += stats.videoStreams;
        _stats.largestConference = std::max(stats.transports, _stats.largestConference);
    }

    _stats.engine = _engine.getStats();

    if (_mainAllocator.size() < 512)
    {
        logger::warn("stats main pool %zu, mixers %zu", "MixerManager", _mainAllocator.size(), _mixers.size());
    }
}

Stats::AggregatedBarbellStats MixerManager::getBarbellStats()
{
    Stats::AggregatedBarbellStats result;

    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto now = utils::Time::getAbsoluteTime();

        for (const auto& mixer : _mixers)
        {
            result._stats.emplace(mixer.second->getId(), mixer.second->gatherBarbellStats(now));
        }
    }

    return result;
}

} // namespace bridge
