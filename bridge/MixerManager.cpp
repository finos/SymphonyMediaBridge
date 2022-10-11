#include "bridge/MixerManager.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/Mixer.h"
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
      _engineMessages(16 * 1024),
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

Mixer* MixerManager::create(bool useGlobalPort)
{
    return create(_config.defaultLastN, useGlobalPort);
}

Mixer* MixerManager::create(uint32_t lastN, bool useGlobalPort)
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

    auto engineMixerEmplaceResult = _engineMixers.emplace(id,
        std::make_unique<EngineMixer>(id,
            _jobManager,
            *this,
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
            engineMixerEmplaceResult.first->second->getLoggableId().getInstanceId(),
            _transportFactory,
            _engine,
            *(engineMixerEmplaceResult.first->second),
            _idGenerator,
            _ssrcGenerator,
            _config,
            audioSsrcs,
            videoSsrcs,
            videoPinSsrcs,
            useGlobalPort));
    if (!mixerEmplaceResult.second)
    {
        logger::error("Failed to create mixer", "MixerManager");
        _engineMixers.erase(id);
        return nullptr;
    }

    {
        EngineCommand::Command addMixerCommand = {EngineCommand::Type::AddMixer};
        addMixerCommand.command.addMixer.mixer = engineMixerEmplaceResult.first->second.get();
        _engine.pushCommand(std::move(addMixerCommand));
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

    EngineCommand::Command command(EngineCommand::Type::RemoveMixer);
    command.command.removeMixer.mixer = _engineMixers[id].get();
    _engine.pushCommand(std::move(command));
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
                EngineCommand::Command command(EngineCommand::Type::RemoveMixer);
                command.command.removeMixer.mixer = _engineMixers[it->first].get();
                _engine.pushCommand(std::move(command));
            }
        }
    }

    for (;; usleep(10000))
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        if (_mixers.empty())
            break;
    }

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
            auto timestamp = utils::Time::getAbsoluteTime();
            pacer.tick(timestamp);

            for (EngineMessage::Message nextMessage; _engineMessages.pop(nextMessage);)
            {
                switch (nextMessage.type)
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
                    engineMessageSctp(std::move(nextMessage));
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

                case EngineMessage::Type::BarbellRemoved:
                    engineBarbellRemoved(nextMessage.command.barbellMessage);
                    break;
                default:
                    assert(false);
                    break;
                }
            }

            if (++_stats.ticksSinceLastUpdate >= 50)
            {
                _transportFactory.maintenance(timestamp);
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

bool MixerManager::onMessage(EngineMessage::Message&& message)
{
    auto messagePosted = _engineMessages.push(std::move(message));
    assert(messagePosted);
    return messagePosted;
}

void MixerManager::engineMessageMixerRemoved(const EngineMessage::Message& message)
{
    // Aims to delete the mixer out of the locker at it can take some time
    // and we want to reduce the lock contention
    std::unique_ptr<bridge::Mixer> mixer;

    {
        std::lock_guard<std::mutex> locker(_configurationLock);

        auto& command = message.command.mixerRemoved;

        std::string mixerId(command.mixer->getId()); // copy id string to have it after EngineMixer is deleted
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

        logger::info("Removing EngineMixer %s", "MixerManager", command.mixer->getLoggableId().c_str());
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

    auto& mixerAudioBuffers = _audioBuffers[message.command.allocateAudioBuffer.mixer->getId()];
    auto findResult = mixerAudioBuffers.find(message.command.allocateAudioBuffer.ssrc);
    if (findResult != mixerAudioBuffers.cend())
    {
        return;
    }

    logger::info("Allocating audio buffer for EngineMixer %s, ssrc %u",
        "MixerManager",
        message.command.allocateAudioBuffer.mixer->getLoggableId().c_str(),
        message.command.allocateAudioBuffer.ssrc);

    auto audioBuffer = std::make_unique<EngineMixer::AudioBuffer>();
    {
        EngineCommand::Command command(EngineCommand::Type::AddAudioBuffer);
        command.command.addAudioBuffer.mixer = message.command.allocateAudioBuffer.mixer;
        command.command.addAudioBuffer.ssrc = message.command.allocateAudioBuffer.ssrc;
        command.command.addAudioBuffer.audioBuffer = audioBuffer.get();
        _engine.pushCommand(std::move(command));
    }
    mixerAudioBuffers.emplace(message.command.allocateAudioBuffer.ssrc, std::move(audioBuffer));
}

void MixerManager::engineMessageAudioStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.audioStreamRemoved;
    logger::info("Removing audioStream endpointId %s from mixer %s",
        "MixerManager",
        command.engineStream->endpointId.c_str(),
        command.mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command.mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command.mixer->getLoggableId().c_str(),
            command.mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineAudioStreamRemoved(command.engineStream);
}

void MixerManager::engineMessageVideoStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.videoStreamRemoved;
    logger::info("Removing videoStream endpointId %s from mixer %s",
        "MixerManager",
        command.engineStream->endpointId.c_str(),
        command.mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command.mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command.mixer->getLoggableId().c_str(),
            command.mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineVideoStreamRemoved(command.engineStream);
}

void MixerManager::engineMessageRecordingStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.recordingStreamRemoved;
    logger::info("Removing recordingStream  %s from mixer %s",
        "MixerManager",
        command.engineStream->id.c_str(),
        command.mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command.mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command.mixer->getLoggableId().c_str(),
            command.mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingStreamRemoved(command.engineStream);
}

void MixerManager::engineMessageDataStreamRemoved(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.dataStreamRemoved;
    logger::info("Removing dataStream endpointId %s from mixer %s",
        "MixerManager",
        command.engineStream->endpointId.c_str(),
        command.mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command.mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command.mixer->getLoggableId().c_str(),
            command.mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineDataStreamRemoved(command.engineStream);
}

void MixerManager::engineMessageMixerTimedOut(const EngineMessage::Message& message)
{
    const auto& command = message.command.mixerTimedOut;
    logger::info("Mixer %s inactivity time out. Deleting it.", "MixerManager", command.mixer->getLoggableId().c_str());
    remove(command.mixer->getId());
}

void MixerManager::engineMessageInboundSsrcRemoved(const EngineMessage::Message& message)
{
    const auto& command = message.command.ssrcInboundRemoved;
    logger::info("Mixer %s removed ssrc %u", "MixerManager", command.mixer->getLoggableId().c_str(), command.ssrc);
    delete command.opusDecoder;
}

void MixerManager::engineMessageAllocateVideoPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.allocateVideoPacketCache;
    const auto mixerItr = _mixers.find(command.mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateVideoPacketCache(command.ssrc, command.endpointIdHash);
}

void MixerManager::engineMessageFreeVideoPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.freeVideoPacketCache;
    const auto mixerItr = _mixers.find(command.mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeVideoPacketCache(command.ssrc, command.endpointIdHash);
}

void MixerManager::engineMessageSctp(EngineMessage::Message&& message)
{
    const auto& sctpMessage = message.command.sctpMessage;
    auto& sctpHeader = webrtc::streamMessageHeader(*message.packet);

    if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
    {
        // create command with this packet to send the binary data -> engine -> WebRtcDataStream belonging to this
        // transport
        EngineCommand::Command command{EngineCommand::Type::SctpControl};
        auto& sctpControl = command.command.sctpControl;
        command.packet.swap(message.packet);
        sctpControl.mixer = sctpMessage.mixer;
        sctpControl.endpointIdHash = sctpMessage.endpointIdHash;
        _engine.pushCommand(std::move(command));
        return; // do not free packet as we passed it on
    }
    else if (sctpHeader.payloadProtocol == webrtc::DataChannelPpid::WEBRTC_STRING)
    {
        std::string body(reinterpret_cast<const char*>(sctpHeader.data()),
            message.packet->getLength() - sizeof(sctpHeader));

        try
        {
            auto json = nlohmann::json::parse(body);
            if (api::DataChannelMessageParser::isPinnedEndpointsChanged(json))
            {
                auto mixerIt = _mixers.find(sctpMessage.mixer->getId());
                if (mixerIt != _mixers.end())
                {
                    const auto& pinnedEndpoints = api::DataChannelMessageParser::getPinnedEndpoint(json);
                    if (pinnedEndpoints.empty())
                    {
                        mixerIt->second->unpinEndpoint(sctpMessage.endpointIdHash);
                    }
                    else
                    {
                        mixerIt->second->pinEndpoint(sctpMessage.endpointIdHash, pinnedEndpoints[0]);
                    }
                }
            }
            else if (api::DataChannelMessageParser::isEndpointMessage(json))
            {
                auto mixerIt = _mixers.find(sctpMessage.mixer->getId());
                if (mixerIt == _mixers.end())
                {
                    return;
                }

                const auto toItr = api::DataChannelMessageParser::getEndpointMessageTo(json);
                const auto payloadItr = api::DataChannelMessageParser::getEndpointMessagePayload(json);
                if (toItr == json.end() || payloadItr == json.end())
                {
                    return;
                }

                mixerIt->second->sendEndpointMessage(toItr->get<std::string>(),
                    sctpMessage.endpointIdHash,
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
}

void MixerManager::engineRecordingStopped(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.recordingStopped;
    logger::info("Stopping recording %s from mixer %s",
        "MixerManager",
        command.recordingDesc->recordingId.c_str(),
        command.mixer->getLoggableId().c_str());

    const auto mixerIter = _mixers.find(command.mixer->getId());
    if (mixerIter == _mixers.cend())
    {
        logger::info("Mixer %s (id=%s) does not exist",
            "MixerManager",
            command.mixer->getLoggableId().c_str(),
            command.mixer->getId().c_str());
        return;
    }

    mixerIter->second->engineRecordingDescStopped(*command.recordingDesc);
}

void MixerManager::engineMessageAllocateRecordingRtpPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.allocateRecordingRtpPacketCache;
    const auto mixerItr = _mixers.find(command.mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->allocateRecordingRtpPacketCache(command.ssrc, command.endpointIdHash);
}

void MixerManager::engineMessageFreeRecordingRtpPacketCache(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.freeRecordingRtpPacketCache;
    const auto mixerItr = _mixers.find(command.mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->freeRecordingRtpPacketCache(command.ssrc, command.endpointIdHash);
}

void MixerManager::engineMessageRemoveRecordingTransport(const EngineMessage::Message& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto& command = message.command.removeRecordingTransport;
    const auto mixerItr = _mixers.find(command.mixer->getId());
    if (mixerItr == _mixers.cend())
    {
        return;
    }

    mixerItr->second->removeRecordingTransport(command.streamId, command.endpointIdHash);
}

void MixerManager::engineBarbellRemoved(const EngineMessage::EngineBarbellMessage& message)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto mixerIt = _mixers.find(message.mixer->getId());
    if (mixerIt != _mixers.end())
    {
        mixerIt->second->engineBarbellRemoved(message.barbell);
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

    result.jobQueueLength = _jobManager.getCount();
    result.receivePoolSize = _mainAllocator.size();
    result.sendPoolSize = _sendAllocator.size();
    result.udpSharedEndpointsSendQueue = udpMetrics.sendQueue;
    result.udpSharedEndpointsReceiveKbps = static_cast<uint32_t>(udpMetrics.receiveKbps);
    result.udpSharedEndpointsSendKbps = static_cast<uint32_t>(udpMetrics.sendKbps);

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
    _stats.ticksSinceLastUpdate = 0;

    if (_mainAllocator.size() < 512)
    {
        logger::warn("stats main pool %zu, mixers %zu", "MixerManager", _mainAllocator.size(), _mixers.size());
    }
}

} // namespace bridge
