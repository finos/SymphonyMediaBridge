#include "bridge/MixerManager.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/Mixer.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
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
#include "utils/Time.h"
#include "webrtc/DataChannel.h"
#include <vector>

namespace
{

const auto intervalNs = 10000000UL;
const uint32_t maxLastN = 16;

} // namespace

namespace bridge
{

MixerManager::MixerManager(utils::IdGenerator& idGenerator,
    utils::SsrcGenerator& ssrcGenerator,
    jobmanager::JobManager& jobManager,
    transport::TransportFactory& transportFactory,
    Engine& engine,
    const config::Config& config,
    memory::PacketPoolAllocator& mainAllocator,
    memory::PacketPoolAllocator& sendAllocator,
    memory::AudioPacketPoolAllocator& audioAllocator)
    : _idGenerator(idGenerator),
      _ssrcGenerator(ssrcGenerator),
      _jobManager(jobManager),
      _transportFactory(transportFactory),
      _engine(engine),
      _config(config),
      _threadRunning(true),
      _engineMessages(16384),
      _mainAllocator(mainAllocator),
      _sendAllocator(sendAllocator),
      _audioAllocator(audioAllocator)
{
    _engine.setMessageListener(this);

    // thread must initialize last to ensure the thread observes other members fully initialized
    _managerThread = std::make_unique<std::thread>([this] { this->run(); });
}

MixerManager::~MixerManager()
{
    logger::debug("Deleted", "MixerManager");
}

Mixer* MixerManager::create()
{
    return create(_config.defaultLastN);
}

Mixer* MixerManager::create(uint32_t lastN)
{
    lastN = std::min(lastN, maxLastN);
    logger::info("Create mixer, last-n %u", "MixerManager", lastN);

    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto id = std::to_string(_idGenerator.next());
    const auto localVideoSsrc = _ssrcGenerator.next();

    std::vector<uint32_t> audioSsrcs;
    std::vector<SimulcastLevel> videoSsrcs;
    std::vector<SimulcastLevel> videoPinSsrcs;

    // Last-n + extra
    for (uint32_t i = 0; i < lastN + 2; ++i)
    {
        audioSsrcs.push_back(_ssrcGenerator.next());
    }

    // Last-n + screen share, extra
    for (uint32_t i = 0; i < lastN + 3; ++i)
    {
        videoSsrcs.push_back({_ssrcGenerator.next(), _ssrcGenerator.next()});
    }

    for (uint32_t i = 0; i < 4; ++i)
    {
        videoPinSsrcs.push_back({_ssrcGenerator.next(), _ssrcGenerator.next()});
    }

    auto engineMixerEmplaceResult = _engineMixers.emplace(id,
        std::make_unique<EngineMixer>(id,
            _jobManager,
            *this,
            _config.mixerInactivityTimeoutMs,
            localVideoSsrc,
            _config,
            _sendAllocator,
            _audioAllocator,
            audioSsrcs,
            videoSsrcs,
            lastN));
    if (!engineMixerEmplaceResult.second)
    {
        logger::error("Failed to create engineMixer", "MixerManager");
        return nullptr;
    }

    auto mixerEmplaceResult = _mixers.emplace(id,
        std::make_unique<Mixer>(id,
            _transportFactory,
            _engine,
            *(engineMixerEmplaceResult.first->second),
            _idGenerator,
            _ssrcGenerator,
            _config,
            audioSsrcs,
            videoSsrcs,
            videoPinSsrcs));
    if (!mixerEmplaceResult.second)
    {
        logger::error("Failed to create mixer", "MixerManager");
        _engineMixers.erase(id);
        return nullptr;
    }

    {
        EngineCommand::Command addMixerCommand = {EngineCommand::Type::AddMixer};
        addMixerCommand._command.addMixer._mixer = engineMixerEmplaceResult.first->second.get();
        _engine.pushCommand(addMixerCommand);
    }

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

    EngineCommand::Command command = {EngineCommand::Type::RemoveMixer};
    command._command.removeMixer._mixer = _engineMixers[id].get();
    _engine.pushCommand(command);
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
    if (!_threadRunning)
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
                EngineCommand::Command command = {EngineCommand::Type::RemoveMixer};
                command._command.removeMixer._mixer = _engineMixers[it->first].get();
                _engine.pushCommand(command);
            }
        }
    }

    for (;; usleep(10000))
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        if (_mixers.empty())
            break;
    }

    std::lock_guard<std::mutex> locker(_configurationLock);
    _threadRunning = false;
    _managerThread->join();
    logger::info("MixerManager thread stopped", "MixerManager");
}

void MixerManager::run()
{
    logger::info("MixerManager thread started", "MixerManager");
    concurrency::setThreadName("MixerManager");
    utils::Pacer pacer(intervalNs);

    while (_threadRunning)
    {
        try
        {
            pacer.tick(utils::Time::getAbsoluteTime());

            EngineMessage::Message nextMessage;
            for (auto result = _engineMessages.pop(nextMessage); result; result = _engineMessages.pop(nextMessage))
            {
                switch (nextMessage._type)
                {
                case EngineMessage::Type::MixerRemoved:
                    engineMessageMixerRemoved(nextMessage);
                    break;

                case EngineMessage::Type::AllocateAudioBuffer:
                    engineMessageAllocateAudioBuffer(nextMessage);
                    break;

                case EngineMessage::Type::AudioStreamRemoved:
                    engineMessageAudioStreamRemoved(nextMessage);
                    break;

                case EngineMessage::Type::VideoStreamRemoved:
                    engineMessageVideoStreamRemoved(nextMessage);
                    break;

                case EngineMessage::Type::DataStreamRemoved:
                    engineMessageDataStreamRemoved(nextMessage);
                    break;
                case EngineMessage::Type::RecordingStreamRemoved:
                    engineMessageRecordingStreamRemoved(nextMessage);
                    break;

                case EngineMessage::Type::MixerTimedOut:
                    engineMessageMixerTimedOut(nextMessage);
                    break;

                case EngineMessage::Type::SctpMessage:
                    engineMessageSctp(nextMessage);
                    break;

                case EngineMessage::Type::AllocateVideoPacketCache:
                    engineMessageAllocateVideoPacketCache(nextMessage);
                    break;
                case EngineMessage::Type::InboundSsrcRemoved:
                    engineMessageInboundSsrcRemoved(nextMessage);
                    break;

                case EngineMessage::Type::FreeVideoPacketCache:
                    engineMessageFreeVideoPacketCache(nextMessage);
                    break;

                case EngineMessage::Type::RecordingStopped:
                    engineRecordingStopped(nextMessage);
                    break;

                case EngineMessage::Type::AllocateRecordingRtpPacketCache:
                    engineMessageAllocateRecordingRtpPacketCache(nextMessage);
                    break;
                case EngineMessage::Type::FreeRecordingRtpPacketCache:
                    engineMessageFreeRecordingRtpPacketCache(nextMessage);
                    break;

                case EngineMessage::Type::RemoveRecordingTransport:
                    engineMessageRemoveRecordingTransport(nextMessage);
                    break;

                default:
                    assert(false);
                    break;
                }
            }

            if (++_stats._ticksSinceLastUpdate >= 50)
            {
                updateStats();
            }
        }
        catch (std::exception e)
        {
            logger::error("Exception in MixerManager run: %s", "MixerManager", e.what());
        }
        catch (...)
        {
            logger::error("Unknown exception in MixerManager run", "MixerManager");
        }

        const auto toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());
        utils::Time::nanoSleep(toSleep);
    }
}

void MixerManager::onMessage(const EngineMessage::Message& message)
{
    auto rc = _engineMessages.push(message);
    assert(rc);
}

void MixerManager::engineMessageMixerRemoved(const EngineMessage::Message& message)
{
    // Aims to delete the mixer out of the locker at it can take some time
    // and we want to reduce the lock contention
    std::unique_ptr<bridge::Mixer> mixer;

    {
        std::lock_guard<std::mutex> locker(_configurationLock);

        auto& command = message._command.mixerRemoved;

        std::string mixerId(command._mixer->getId()); // copy id string to have it after EngineMixer is deleted
        auto findResult = _mixers.find(mixerId);
        if (findResult != _mixers.end())
        {
            mixer = std::move(findResult->second);
            mixer->stopTransports(); // this will stop new packets from coming in
            if (!mixer->waitForAllPendingJobs(1000))
            {
                logger::error("still pending jobs or packets after 1s.", mixer->getLoggableId().c_str());
            }

            _mixers.erase(findResult);
        }
        else
        {
            logger::debug("did not find mixer to stop %s", "MixerManager", mixerId.c_str());
        }

        logger::info("Removing EngineMixer %s", "MixerManager", command._mixer->getLoggableId().c_str());
        // This will sweep the PacketPoolAllocator. Any pending jobs may crash or corrupt memory.
        _engineMixers.erase(mixerId);

        auto mixerAudioBuffers = _audioBuffers.find(mixerId);
        if (mixerAudioBuffers != _audioBuffers.cend())
        {
            mixerAudioBuffers->second.clear();
            _audioBuffers.erase(mixerAudioBuffers);
        }
    }

    mixer.reset();
    logger::info("EngineMixer removed", "MixerManager");

    if (_engineMixers.empty())
    {
        _mainAllocator.logAllocatedElements();
        _sendAllocator.logAllocatedElements();
        _audioAllocator.logAllocatedElements();
    }
}

void MixerManager::engineMessageAllocateAudioBuffer(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto& mixerAudioBuffers = _audioBuffers[message._command.allocateAudioBuffer._mixer->getId()];
    auto findResult = mixerAudioBuffers.find(message._command.allocateAudioBuffer._ssrc);
    if (findResult != mixerAudioBuffers.cend())
    {
        return;
    }

    logger::info("Allocating audio buffer for EngineMixer %s, ssrc %u",
        "MixerManager",
        message._command.allocateAudioBuffer._mixer->getLoggableId().c_str(),
        message._command.allocateAudioBuffer._ssrc);

    auto audioBuffer = std::make_unique<EngineMixer::AudioBuffer>();
    {
        EngineCommand::Command command = {EngineCommand::Type::AddAudioBuffer};
        command._command.addAudioBuffer._mixer = message._command.allocateAudioBuffer._mixer;
        command._command.addAudioBuffer._ssrc = message._command.allocateAudioBuffer._ssrc;
        command._command.addAudioBuffer._audioBuffer = audioBuffer.get();
        _engine.pushCommand(command);
    }
    mixerAudioBuffers.emplace(message._command.allocateAudioBuffer._ssrc, std::move(audioBuffer));
}

void MixerManager::engineMessageAudioStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.audioStreamRemoved;
    logger::info("Removing audioStream endpointId %s from mixer %s",
        "MixerManager",
        command._engineStream->_endpointId.c_str(),
        command._mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command._mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command._mixer->getLoggableId().c_str(),
            command._mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineAudioStreamRemoved(command._engineStream);
}

void MixerManager::engineMessageVideoStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.videoStreamRemoved;
    logger::info("Removing videoStream endpointId %s from mixer %s",
        "MixerManager",
        command._engineStream->_endpointId.c_str(),
        command._mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command._mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command._mixer->getLoggableId().c_str(),
            command._mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineVideoStreamRemoved(command._engineStream);
}

void MixerManager::engineMessageRecordingStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.recordingStreamRemoved;
    logger::info("Removing recordingStream  %s from mixer %s",
        "MixerManager",
        command._engineStream->_id.c_str(),
        command._mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command._mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command._mixer->getLoggableId().c_str(),
            command._mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingStreamRemoved(command._engineStream);
}

void MixerManager::engineMessageDataStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.dataStreamRemoved;
    logger::info("Removing dataStream endpointId %s from mixer %s",
        "MixerManager",
        command._engineStream->_endpointId.c_str(),
        command._mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command._mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command._mixer->getLoggableId().c_str(),
            command._mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineDataStreamRemoved(command._engineStream);
}

void MixerManager::engineMessageMixerTimedOut(const EngineMessage::Message& message)
{
    const auto& command = message._command.mixerTimedOut;
    logger::info("Mixer %s inactivity time out. Deleting it.", "MixerManager", command._mixer->getLoggableId().c_str());
    remove(command._mixer->getId());
}

void MixerManager::engineMessageInboundSsrcRemoved(const EngineMessage::Message& message)
{
    const auto& command = message._command.ssrcInboundRemoved;
    logger::info("Mixer %s removed ssrc %u", "MixerManager", command._mixer->getLoggableId().c_str(), command._ssrc);
    delete command._opusDecoder;
}

void MixerManager::engineMessageAllocateVideoPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.allocateVideoPacketCache;
    const auto mixerItr = _mixers.find(command._mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateVideoPacketCache(command._ssrc, command._endpointIdHash);
}

void MixerManager::engineMessageFreeVideoPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.freeVideoPacketCache;
    const auto mixerItr = _mixers.find(command._mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeVideoPacketCache(command._ssrc, command._endpointIdHash);
}

void MixerManager::engineMessageSctp(const EngineMessage::Message& message)
{
    const auto& sctpMessage = message._command.sctpMessage;
    auto& sctpHeader = webrtc::streamMessageHeader(*sctpMessage._message);

    if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
    {
        // create command with this packet to send the binary data -> engine -> WebRtcDataStream belonging to this
        // transport
        EngineCommand::Command command{EngineCommand::Type::SctpControl};
        auto& sctpControl = command._command.sctpControl;
        sctpControl._message = sctpMessage._message;
        sctpControl._allocator = sctpMessage._allocator;
        sctpControl._mixer = sctpMessage._mixer;
        sctpControl._endpointIdHash = sctpMessage._endpointIdHash;
        _engine.pushCommand(command);
        return; // do not free packet as we passed it on
    }
    else if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_STRING)
    {
        std::string body(reinterpret_cast<const char*>(sctpHeader.data()),
            sctpMessage._message->getLength() - sizeof(sctpHeader));

        try
        {
            auto json = nlohmann::json::parse(body);
            if (api::DataChannelMessageParser::isPinnedEndpointsChanged(json))
            {
                auto mixerIt = _mixers.find(sctpMessage._mixer->getId());
                if (mixerIt != _mixers.end())
                {
                    const auto& pinnedEndpoints = api::DataChannelMessageParser::getPinnedEndpoint(json);
                    if (pinnedEndpoints.empty())
                    {
                        mixerIt->second->unpinEndpoint(sctpMessage._endpointIdHash);
                    }
                    else
                    {
                        mixerIt->second->pinEndpoint(sctpMessage._endpointIdHash, pinnedEndpoints[0]);
                    }
                }
            }
            else if (api::DataChannelMessageParser::isEndpointMessage(json))
            {
                auto mixerIt = _mixers.find(sctpMessage._mixer->getId());
                if (mixerIt == _mixers.end())
                {
                    sctpMessage._allocator->free(sctpMessage._message);
                    return;
                }

                const auto toItr = api::DataChannelMessageParser::getEndpointMessageTo(json);
                const auto payloadItr = api::DataChannelMessageParser::getEndpointMessagePayload(json);
                if (toItr == json.end() || payloadItr == json.end())
                {
                    sctpMessage._allocator->free(sctpMessage._message);
                    return;
                }

                mixerIt->second->sendEndpointMessage(toItr->get<std::string>(),
                    sctpMessage._endpointIdHash,
                    payloadItr->dump());
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
    sctpMessage._allocator->free(sctpMessage._message);
}

void MixerManager::engineRecordingStopped(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.recordingStopped;
    logger::info("Stopping recording %s from mixer %s",
        "MixerManager",
        command._recordingDesc->_recordingId.c_str(),
        command._mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command._mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command._mixer->getLoggableId().c_str(),
            command._mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingDescStopped(*command._recordingDesc);
}

void MixerManager::engineMessageAllocateRecordingRtpPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.allocateRecordingRtpPacketCache;
    const auto mixerItr = _mixers.find(command._mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateRecordingRtpPacketCache(command._ssrc, command._endpointIdHash);
}

void MixerManager::engineMessageFreeRecordingRtpPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.freeRecordingRtpPacketCache;
    const auto mixerItr = _mixers.find(command._mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeRecordingRtpPacketCache(command._ssrc, command._endpointIdHash);
}

void MixerManager::engineMessageRemoveRecordingTransport(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message._command.removeRecordingTransport;
    const auto mixerItr = _mixers.find(command._mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->removeRecordingTransport(command._streamId, command._endpointIdHash);
}

// This method may block up to 1s to collect the statistics
Stats::MixerManagerStats MixerManager::getStats()
{
    Stats::MixerManagerStats result;
    auto systemStats = _systemStatCollector.collect();

    {
        std::lock_guard<std::mutex> locker(_configurationLock);

        result._conferences = _stats._conferences;
        result._videoStreams = _stats._videoStreams;
        result._audioStreams = _stats._audioStreams;
        result._dataStreams = _stats._dataStreams;
        result._engineStats = _stats._engine;
        result._systemStats = systemStats;
    }

    EndpointMetrics udpMetrics = _transportFactory.getSharedUdpEndpointsMetrics();

    result._jobQueueLength = _jobManager.getCount();
    result._receivePoolSize = _mainAllocator.size();
    result._sendPoolSize = _sendAllocator.size();
    result._udpSharedEndpointsSendQueue = udpMetrics.sendQueue;
    result._udpSharedEndpointsReceiveKbps = static_cast<uint32_t>(udpMetrics.receiveKbps);
    result._udpSharedEndpointsSendKbps = static_cast<uint32_t>(udpMetrics.sendKbps);

    return result;
}

void MixerManager::updateStats()
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    _stats._conferences = _mixers.size();
    _stats._videoStreams = 0;
    _stats._audioStreams = 0;
    _stats._dataStreams = 0;

    for (const auto& mixer : _mixers)
    {
        const auto stats = mixer.second->getStats();
        _stats._videoStreams += stats._videoStreams;
        _stats._audioStreams += stats._audioStreams;
        _stats._dataStreams += stats._videoStreams;
    }

    _stats._engine = _engine.getStats();
    _stats._ticksSinceLastUpdate = 0;

    if (_mainAllocator.size() < 512)
    {
        logger::warn("stats main pool %zu, mixers %zu", "MixerManager", _mainAllocator.size(), _mixers.size());
    }
}

} // namespace bridge
