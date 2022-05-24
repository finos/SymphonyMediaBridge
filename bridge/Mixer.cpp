#include "bridge/Mixer.h"
#include "api/RecordingChannel.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/StreamDescription.h"
#include "bridge/TransportDescription.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/PacketCache.h"
#include "config/Config.h"
#include "jobmanager/JobManager.h"
#include "logger/Logger.h"
#include "transport/TransportFactory.h"
#include "utils/IdGenerator.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include "utils/StringBuilder.h"
#include <unistd.h>
#include <utility>

namespace
{

void logTransportPacketLoss(const std::string& endpointId, transport::RtcTransport& transport, const char* mixerId)
{
    const auto lossCount = transport.getSenderLossCount();
    if (lossCount > 0)
    {
        logger::info("EndpointId %s %s far side reports %u packets lost",
            mixerId,
            endpointId.c_str(),
            transport.getLoggableId().c_str(),
            lossCount);
    }

    std::unordered_map<uint32_t, transport::ReportSummary> reportSummaries;
    transport.getReportSummary(reportSummaries);

    for (const auto& reportSummaryEntry : reportSummaries)
    {
        const auto ssrc = reportSummaryEntry.first;
        const auto& reportSummary = reportSummaryEntry.second;

        logger::info(
            "EndpointId %s %s, outbound ssrc %u, packets %u"
            ", last sent seq %u, last received %u, reported loss count %u, initial RTP timestamp %u, RTP timestamp %u",
            mixerId,
            endpointId.c_str(),
            transport.getLoggableId().c_str(),
            ssrc,
            reportSummary.packetsSent,
            reportSummary.sequenceNumberSent,
            reportSummary.extendedSeqNoReceived,
            reportSummary.lostPackets,
            reportSummary.initialRtpTimestamp,
            reportSummary.rtpTimestamp);
    }

    auto audioStats = transport.getCumulativeAudioReceiveCounters();
    auto videoStats = transport.getCumulativeVideoReceiveCounters();
    if (audioStats.getPacketsReceived() > 0)
    {
        logger::info("EndpointId %s %s, inbound audio packets received %" PRIu64 ", lost %" PRIu64 ", octets %" PRIu64
                     "B",
            mixerId,
            endpointId.c_str(),
            transport.getLoggableId().c_str(),
            audioStats.getPacketsReceived(),
            audioStats.lostPackets,
            audioStats.octets);
    }

    if (videoStats.getPacketsReceived() > 0)
    {
        logger::info("EndpointId %s %s, inbound video packets received %" PRIu64 ", lost %" PRIu64 ", octets %" PRIu64
                     "B",
            mixerId,
            endpointId.c_str(),
            transport.getLoggableId().c_str(),
            videoStats.getPacketsReceived(),
            videoStats.lostPackets,
            videoStats.octets);
    }
}

void makeSsrcWhitelistLog(const bridge::SsrcWhitelist& ssrcWhitelist, utils::StringBuilder<256>& outLogString)
{
    outLogString.append("ssrcWhitelist: ");
    if (!ssrcWhitelist._enabled)
    {
        outLogString.append("disabled");
        return;
    }

    outLogString.append("enabled ");
    outLogString.append(ssrcWhitelist._numSsrcs);

    switch (ssrcWhitelist._numSsrcs)
    {
    case 1:
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist._ssrcs[0]);
        break;

    case 2:
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist._ssrcs[0]);
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist._ssrcs[1]);
        break;

    default:
        break;
    }
}

bool waitForPendingJobs(const uint32_t timeoutMs, const uint32_t pollIntervalMs, transport::Transport& transport)
{
    uint32_t totalSleepTimeMs = 0;
    while (totalSleepTimeMs < timeoutMs && transport.hasPendingJobs())
    {
        totalSleepTimeMs += pollIntervalMs;
        usleep(pollIntervalMs * 1000);
    }

    return totalSleepTimeMs < timeoutMs;
}

} // namespace

namespace bridge
{

Mixer::Mixer(std::string id,
    transport::TransportFactory& transportFactory,
    Engine& engine,
    EngineMixer& engineMixer,
    utils::IdGenerator& idGenerator,
    utils::SsrcGenerator& ssrcGenerator,
    const config::Config& config,
    const std::vector<uint32_t>& audioSsrcs,
    const std::vector<SimulcastLevel>& videoSsrcs,
    const std::vector<SimulcastLevel>& videoPinSsrcs)
    : _config(config),
      _id(std::move(id)),
      _loggableId("Mixer"),
      _markedForDeletion(false),
      _audioSsrcs(audioSsrcs),
      _videoSsrcs(videoSsrcs),
      _videoPinSsrcs(videoPinSsrcs),
      _transportFactory(transportFactory),
      _engine(engine),
      _engineMixer(engineMixer),
      _idGenerator(idGenerator),
      _ssrcGenerator(ssrcGenerator)
{
    logger::info("id=%s, served by engine mixer %s",
        _loggableId.c_str(),
        _id.c_str(),
        engineMixer.getLoggableId().c_str());
}

void Mixer::markForDeletion()
{
    _markedForDeletion = true;
}

void Mixer::stopTransports()
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    logger::debug("stopping transports %zu %zu %zu",
        _loggableId.c_str(),
        _bundleTransports.size(),
        _audioStreams.size(),
        _videoStreams.size());

    for (auto& bundleTransportEntry : _bundleTransports)
    {
        assert(bundleTransportEntry.second._transport.get());
        logTransportPacketLoss("", *bundleTransportEntry.second._transport, _loggableId.c_str());

        bundleTransportEntry.second._transport->stop();
        if (!waitForPendingJobs(700, 5, *bundleTransportEntry.second._transport))
        {
            logger::error("Transport for endpointId %s did not finish pending jobs in time. count=%u. Continuing "
                          "deletion anyway.",
                _loggableId.c_str(),
                bundleTransportEntry.first.c_str(),
                bundleTransportEntry.second._transport->getJobCounter().load());
        }
    }

    for (auto& audioStreamEntry : _audioStreams)
    {
        assert(audioStreamEntry.second->_transport);
        if (_bundleTransports.find(audioStreamEntry.second->_endpointId) != _bundleTransports.end())
        {
            continue;
        }

        logTransportPacketLoss("", *audioStreamEntry.second->_transport, _loggableId.c_str());
        audioStreamEntry.second->_transport->stop();
        if (!waitForPendingJobs(700, 5, *audioStreamEntry.second->_transport))
        {
            logger::error("Transport for endpointId %s did not finish pending jobs in time. Continuing "
                          "deletion anyway.",
                _loggableId.c_str(),
                audioStreamEntry.second->_endpointId.c_str());
        }
    }

    for (auto& videoStreamEntry : _videoStreams)
    {
        assert(videoStreamEntry.second->_transport);
        if (_bundleTransports.find(videoStreamEntry.second->_endpointId) != _bundleTransports.end())
        {
            continue;
        }

        logTransportPacketLoss("", *videoStreamEntry.second->_transport, _loggableId.c_str());
        videoStreamEntry.second->_transport->stop();
        if (!waitForPendingJobs(700, 5, *videoStreamEntry.second->_transport))
        {
            logger::error("Transport for endpointId %s did not finish pending jobs in time. Continuing "
                          "deletion anyway.",
                _loggableId.c_str(),
                videoStreamEntry.second->_endpointId.c_str());
        }
    }
}

bool Mixer::waitForAllPendingJobs(const uint32_t timeoutMs)
{
    uint32_t totalSleepTimeMs = 0;
    const uint32_t waitSlice = 10;
    bool pendingJobs = true;

    while (totalSleepTimeMs < timeoutMs && pendingJobs)
    {
        pendingJobs = false;

        for (auto& audioStream : _audioStreams)
        {
            if (audioStream.second->_transport && audioStream.second->_transport->hasPendingJobs())
            {
                pendingJobs = true;
                break;
            }
        }

        for (auto& videoStream : _videoStreams)
        {
            if (videoStream.second->_transport && videoStream.second->_transport->hasPendingJobs())
            {
                pendingJobs = true;
                break;
            }
        }

        if (pendingJobs)
        {
            usleep(waitSlice * 1000);
            totalSleepTimeMs += waitSlice;
        }

        _engineMixer.flush();
    }

    return totalSleepTimeMs < timeoutMs;
}

bool Mixer::addBundleTransportIfNeeded(const std::string& endpointId, const ice::IceRole iceRole)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_bundleTransports.find(endpointId) != _bundleTransports.end())
    {
        return true;
    }

    const auto endpointIdHash = std::hash<std::string>{}(endpointId);
    const auto emplaceResult =
        _bundleTransports.emplace(endpointId, _transportFactory.create(iceRole, 512, endpointIdHash));
    if (!emplaceResult.second)
    {
        logger::error("Failed to create bundle transport, endpointId %s", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    logger::info("Created bundle transport, endpointId %s, endpointIdHash %lu, transport %s (%p), ice controlling %c",
        _loggableId.c_str(),
        endpointId.c_str(),
        endpointIdHash,
        emplaceResult.first->second._transport->getLoggableId().c_str(),
        emplaceResult.first->second._transport.get(),
        iceRole == ice::IceRole::CONTROLLING ? 't' : 'f');

    return true;
}

bool Mixer::addAudioStream(std::string& outId,
    const std::string& endpointId,
    const utils::Optional<ice::IceRole>& iceRole,
    const bool audioMixed,
    bool rewriteSsrcs)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_audioStreams.find(endpointId) != _audioStreams.end())
    {
        logger::warn("AudioStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());
    auto transport = iceRole.isSet() ? _transportFactory.create(iceRole.get(), 32, std::hash<std::string>{}(endpointId))
                                     : _transportFactory.create(32, std::hash<std::string>{}(endpointId));

    const auto streamItr = _audioStreams.emplace(endpointId,
        std::make_unique<AudioStream>(outId, endpointId, _ssrcGenerator.next(), transport, audioMixed, rewriteSsrcs));
    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created audioStream id %s, endpointId %s, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->_transport->getLoggableId().c_str());

    return streamItr.first->second->_transport->isInitialized();
}

bool Mixer::addVideoStream(std::string& outId,
    const std::string& endpointId,
    const utils::Optional<ice::IceRole>& iceRole,
    bool rewriteSsrcs)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_videoStreams.find(endpointId) != _videoStreams.end())
    {
        logger::warn("VideoStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());
    auto transport = iceRole.isSet() ? _transportFactory.create(iceRole.get(), 32, std::hash<std::string>{}(endpointId))
                                     : _transportFactory.create(32, std::hash<std::string>{}(endpointId));

    const auto emplaceResult = _videoStreams.emplace(endpointId,
        std::make_unique<VideoStream>(outId, endpointId, _ssrcGenerator.next(), transport, rewriteSsrcs));
    if (!emplaceResult.second)
    {
        return false;
    }

    logger::info("Created videoStream id %s, endpointId %s, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        emplaceResult.first->second->_transport->getLoggableId().c_str());

    return emplaceResult.first->second->_transport->isInitialized();
}

bool Mixer::addBundledAudioStream(std::string& outId,
    const std::string& endpointId,
    const bool audioMixed,
    const bool ssrcRewrite)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_audioStreams.find(endpointId) != _audioStreams.end())
    {
        logger::warn("AudioStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());

    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        logger::error("Unable to add bundled stream for non-existing bundle transport, endpointId %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    const auto streamItr = _audioStreams.emplace(endpointId,
        std::make_unique<AudioStream>(outId,
            endpointId,
            _ssrcGenerator.next(),
            transportItr->second._transport,
            audioMixed,
            ssrcRewrite));
    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created bundled audioStream id %s, endpointId %s, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->_transport->getLoggableId().c_str());

    return streamItr.first->second->_transport->isInitialized();
}

bool Mixer::addBundledVideoStream(std::string& outId, const std::string& endpointId, const bool ssrcRewrite)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_videoStreams.find(endpointId) != _videoStreams.end())
    {
        logger::warn("VideoStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());

    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        logger::error("Unable to add bundled stream for non-existing bundle transport, endpointId %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    const auto streamItr = _videoStreams.emplace(endpointId,
        std::make_unique<VideoStream>(outId,
            endpointId,
            _ssrcGenerator.next(),
            transportItr->second._transport,
            ssrcRewrite));
    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created bundled videoStream id %s, endpointId %s, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->_transport->getLoggableId().c_str());

    return streamItr.first->second->_transport->isInitialized();
}

bool Mixer::addBundledDataStream(std::string& outId, const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_dataStreams.find(endpointId) != _dataStreams.end())
    {
        logger::warn("DataStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());

    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        logger::error("Unable to add bundled stream for non-existing bundle transport, endpointId %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    const auto streamItr = _dataStreams.emplace(endpointId,
        std::make_unique<DataStream>(outId, endpointId, transportItr->second._transport));
    if (!streamItr.second)
    {
        return false;
    }
    if (_config.sctp.fixedPort)
    {
        streamItr.first->second->_localSctpPort = 5000;
    }

    logger::info("Created bundled dataStream id %s, endpointId %s, transport %s, port %u",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->_transport->getLoggableId().c_str(),
        streamItr.first->second->_localSctpPort);

    return streamItr.first->second->_transport->isInitialized();
}

bool Mixer::removeAudioStream(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    auto audioStream = audioStreamItr->second.get();

    if (audioStream->_markedForDeletion)
    {
        return true;
    }

    audioStream->_markedForDeletion = true;
    auto engineStreamItr = _audioEngineStreams.find(audioStream->_endpointId);
    if (engineStreamItr == _audioEngineStreams.end())
    {
        return true;
    }

    EngineCommand::Command command;
    command._type = EngineCommand::Type::RemoveAudioStream;
    command._command.removeAudioStream._mixer = &_engineMixer;
    command._command.removeAudioStream._engineStream = engineStreamItr->second.get();
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::removeAudioStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto audioStreamItr = std::find_if(_audioStreams.begin(), _audioStreams.end(), [&](const auto& element) {
            return element.second->_id.compare(id) == 0;
        });

        if (audioStreamItr == _audioStreams.end())
        {
            return false;
        }

        endpointId = audioStreamItr->second->_endpointId;
    }

    return removeAudioStream(endpointId);
}

bool Mixer::removeVideoStream(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    auto videoStream = videoStreamItr->second.get();

    if (videoStream->_markedForDeletion)
    {
        return true;
    }

    videoStream->_markedForDeletion = true;
    auto engineStreamItr = _videoEngineStreams.find(videoStream->_endpointId);
    if (engineStreamItr == _videoEngineStreams.end())
    {
        return true;
    }

    EngineCommand::Command command;
    command._type = EngineCommand::Type::RemoveVideoStream;
    command._command.removeVideoStream._mixer = &_engineMixer;
    command._command.removeVideoStream._engineStream = engineStreamItr->second.get();
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::removeVideoStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto videoStreamItr = std::find_if(_videoStreams.begin(), _videoStreams.end(), [&](const auto& element) {
            return element.second->_id.compare(id) == 0;
        });

        if (videoStreamItr == _videoStreams.end())
        {
            return false;
        }

        endpointId = videoStreamItr->second->_endpointId;
    }

    return removeVideoStream(endpointId);
}

bool Mixer::removeDataStream(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end())
    {
        return false;
    }
    auto dataStream = dataStreamItr->second.get();

    if (dataStream->_markedForDeletion)
    {
        return true;
    }

    dataStream->_markedForDeletion = true;
    auto engineStreamItr = _dataEngineStreams.find(dataStream->_endpointId);
    if (engineStreamItr == _dataEngineStreams.end())
    {
        return true;
    }

    EngineCommand::Command command;
    command._type = EngineCommand::Type::RemoveDataStream;
    command._command.removeDataStream._mixer = &_engineMixer;
    command._command.removeDataStream._engineStream = engineStreamItr->second.get();
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::removeDataStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto dataStreamItr = std::find_if(_dataStreams.begin(), _dataStreams.end(), [&](const auto& element) {
            return element.second->_id.compare(id) == 0;
        });

        if (dataStreamItr == _dataStreams.end())
        {
            return false;
        }

        endpointId = dataStreamItr->second->_endpointId;
    }

    return removeDataStream(endpointId);
}

void Mixer::engineAudioStreamRemoved(EngineAudioStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->_endpointId;
    auto streamItr = _audioStreams.find(endpointId);
    if (streamItr == _audioStreams.end())
    {
        stopTransportIfNeeded(&engineStream->_transport, endpointId);
        _audioEngineStreams.erase(endpointId);

        logger::warn("EngineAudioStream endpointId %s removed, no matching audioStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    auto& stream = streamItr->second;
    logger::info("AudioStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->_id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->_transport;

    _audioStreams.erase(endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _audioEngineStreams.erase(endpointId);
}

void Mixer::engineVideoStreamRemoved(EngineVideoStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->_endpointId;
    auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.end())
    {
        stopTransportIfNeeded(&engineStream->_transport, endpointId);
        _videoEngineStreams.erase(endpointId);

        logger::warn("EngineVideoStream endpointId %s removed, no matching videoStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    auto& stream = streamItr->second;
    logger::info("VideoStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->_id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->_transport;
    _videoStreams.erase(stream->_endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _videoEngineStreams.erase(endpointId);
}

void Mixer::engineDataStreamRemoved(EngineDataStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->_endpointId;
    auto streamItr = _dataStreams.find(endpointId);
    if (streamItr == _dataStreams.end())
    {
        stopTransportIfNeeded(&engineStream->_transport, endpointId);
        _dataEngineStreams.erase(endpointId);

        logger::warn("EngineDataStream endpointId %s removed, no matching dataStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    auto& stream = streamItr->second;

    logger::info("DataStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->_id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->_transport;
    _dataStreams.erase(stream->_endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _dataEngineStreams.erase(endpointId);
}

bool Mixer::getAudioStreamDescription(const std::string& endpointId, StreamDescription& outDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _audioStreams.find(endpointId);
    if (streamItr == _audioStreams.cend())
    {
        return false;
    }

    outDescription = StreamDescription(*streamItr->second);
    if (streamItr->second->_ssrcRewrite)
    {
        for (auto ssrc : _audioSsrcs)
        {
            outDescription._localSsrcs.push_back(ssrc);
        }
    }
    return true;
}

bool Mixer::getVideoStreamDescription(const std::string& endpointId, StreamDescription& outDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.cend())
    {
        return false;
    }

    outDescription = StreamDescription(*streamItr->second);
    if (streamItr->second->_ssrcRewrite)
    {
        for (auto ssrcPair : _videoSsrcs)
        {
            outDescription._localSsrcs.push_back(ssrcPair._ssrc);
            outDescription._localSsrcs.push_back(ssrcPair._feedbackSsrc);
        }
        for (auto ssrcPair : _videoPinSsrcs)
        {
            outDescription._localSsrcs.push_back(ssrcPair._ssrc);
            outDescription._localSsrcs.push_back(ssrcPair._feedbackSsrc);
        }
    }
    return true;
}

bool Mixer::getDataStreamDescription(const std::string& endpointId, DataStreamDescription& outDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _dataStreams.find(endpointId);
    if (streamItr == _dataStreams.cend())
    {
        return false;
    }

    outDescription = DataStreamDescription(*streamItr->second);
    return true;
}

bool Mixer::isAudioStreamConfigured(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _audioStreams.find(endpointId);
    if (streamItr == _audioStreams.cend())
    {
        return false;
    }
    return streamItr->second->_isConfigured;
}

bool Mixer::isVideoStreamConfigured(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.cend())
    {
        return false;
    }
    return streamItr->second->_isConfigured;
}

bool Mixer::isDataStreamConfigured(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _dataStreams.find(endpointId);
    if (streamItr == _dataStreams.cend())
    {
        return false;
    }
    return streamItr->second->_isConfigured;
}

bool Mixer::getTransportBundleDescription(const std::string& endpointId, TransportDescription& outTransportDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto bundleTransportItr = _bundleTransports.find(endpointId);
    if (bundleTransportItr == _bundleTransports.end())
    {
        return false;
    }
    auto bundleTransport = bundleTransportItr->second._transport.get();
    assert(bundleTransport);

    outTransportDescription = TransportDescription(bundleTransport->getLocalCandidates(),
        bundleTransport->getLocalCredentials(),
        bundleTransport->isDtlsClient());

    return true;
}

bool Mixer::getAudioStreamTransportDescription(const std::string& endpointId,
    TransportDescription& outTransportDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }

    auto transport = audioStreamItr->second->_transport.get();
    if (!transport)
    {
        return false;
    }

    if (transport->isIceEnabled() && transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalCandidates(),
            transport->getLocalCredentials(),
            transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort(), transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && !transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort());
    }

    return true;
}

bool Mixer::getVideoStreamTransportDescription(const std::string& endpointId,
    TransportDescription& outTransportDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }

    auto transport = videoStreamItr->second->_transport.get();
    if (!transport)
    {
        return false;
    }

    if (transport->isIceEnabled() && transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalCandidates(),
            transport->getLocalCredentials(),
            transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort(), transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && !transport->isDtlsEnabled())
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort());
    }

    return true;
}

void Mixer::allocateVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto& videoPacketCaches = _videoPacketCaches[endpointIdHash];
    auto findResult = videoPacketCaches.find(ssrc);
    if (findResult != videoPacketCaches.cend())
    {
        return;
    }

    logger::info("Allocating videoPacketCache for ssrc %u, %lu", _loggableId.c_str(), ssrc, endpointIdHash);

    auto videoPacketCache = std::make_unique<PacketCache>("VideoPacketCache", ssrc);
    {
        EngineCommand::Command command(EngineCommand::Type::AddVideoPacketCache);
        command._command.addVideoPacketCache._mixer = &_engineMixer;
        command._command.addVideoPacketCache._ssrc = ssrc;
        command._command.addVideoPacketCache._endpointIdHash = endpointIdHash;
        command._command.addVideoPacketCache._videoPacketCache = videoPacketCache.get();
        _engine.pushCommand(std::move(command));
    }
    videoPacketCaches.emplace(ssrc, std::move(videoPacketCache));
}

void Mixer::freeVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto& videoPacketCaches = _videoPacketCaches[endpointIdHash];
    auto findResult = videoPacketCaches.find(ssrc);
    if (findResult == videoPacketCaches.cend())
    {
        return;
    }

    logger::info("Freeing videoPacketCache for ssrc %u, endpointIdHash %lu", _loggableId.c_str(), ssrc, endpointIdHash);
    videoPacketCaches.erase(ssrc);
}

Mixer::Stats Mixer::getStats()
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    Mixer::Stats result;
    result.videoStreams = _videoStreams.size();
    result.audioStreams = _audioStreams.size();
    result.dataStreams = _dataStreams.size();
    result.transports = _bundleTransports.size();
    return result;
}

bool Mixer::configureAudioStream(const std::string& endpointId,
    const RtpMap& rtpMap,
    const utils::Optional<uint32_t>& remoteSsrc,
    const utils::Optional<uint8_t>& audioLevelExtensionId,
    const utils::Optional<uint8_t>& absSendTimeExtensionId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    auto audioStream = audioStreamItr->second.get();

    if (remoteSsrc.isSet())
    {
        logger::info("Configure audio stream, endpoint id %s, ssrc %u, rtpMap format %u, payloadType %u",
            _loggableId.c_str(),
            endpointId.c_str(),
            remoteSsrc.get(),
            static_cast<uint32_t>(rtpMap._format),
            rtpMap._payloadType);
    }
    else
    {
        logger::info("Configure audio stream, endpoint id %s, ssrc not set, rtpMap format %u, payloadType %u",
            _loggableId.c_str(),
            endpointId.c_str(),
            static_cast<uint32_t>(rtpMap._format),
            rtpMap._payloadType);
    }

    audioStream->_rtpMap = rtpMap;
    audioStream->_remoteSsrc = remoteSsrc;
    audioStream->_rtpMap._audioLevelExtId = audioLevelExtensionId;
    audioStream->_transport->setAudioPayloadType(rtpMap._payloadType, rtpMap._sampleRate);
    audioStream->_rtpMap._absSendTimeExtId = absSendTimeExtensionId;
    if (audioStream->_rtpMap._absSendTimeExtId.isSet())
    {
        audioStream->_transport->setAbsSendTimeExtensionId(audioStream->_rtpMap._absSendTimeExtId.get());
    }
    return true;
}

bool Mixer::reconfigureAudioStream(const std::string& endpointId, const utils::Optional<uint32_t>& remoteSsrc)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end() || !audioStreamItr->second->_isConfigured)
    {
        logger::warn("Reconfigure audio stream, endpoint id %s, %s",
            _loggableId.c_str(),
            endpointId.c_str(),
            audioStreamItr != _audioStreams.end() ? "not configured" : "not found");
        return false;
    }
    auto audioStream = audioStreamItr->second.get();

    if (remoteSsrc.isSet())
    {
        logger::info("Reconfigure audio stream, endpoint id %s, ssrc %u",
            _loggableId.c_str(),
            endpointId.c_str(),
            remoteSsrc.get());
    }
    else
    {
        logger::info("Reconfigure audio stream, endpoint id %s, ssrc not set", _loggableId.c_str(), endpointId.c_str());
    }
    audioStream->_remoteSsrc = remoteSsrc;

    EngineCommand::Command command;
    command._type = EngineCommand::Type::ReconfigureAudioStream;
    command._command.reconfigureAudioStream._mixer = &_engineMixer;
    command._command.reconfigureAudioStream._remoteSsrc = remoteSsrc.isSet() ? remoteSsrc.get() : 0;
    command._command.reconfigureAudioStream._transport = audioStream->_transport.get();
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::configureVideoStream(const std::string& endpointId,
    const RtpMap& rtpMap,
    const RtpMap& feedbackRtpMap,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
    const utils::Optional<uint8_t>& absSendTimeExtensionId,
    const SsrcWhitelist& ssrcWhitelist)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        logger::error("no video stream allocated for end point %s", _loggableId.c_str(), endpointId.c_str());
        return false;
    }
    auto videoStream = videoStreamItr->second.get();

    if (simulcastStream._numLevels > 3)
    {
        logger::error("Simulcast levels can only be 0 - 3. Video stream endpoint id %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    utils::StringBuilder<1024> ssrcsString;
    for (size_t i = 0; i < simulcastStream._numLevels; ++i)
    {
        ssrcsString.append(simulcastStream._levels[i]._ssrc);
        ssrcsString.append(",");
        ssrcsString.append(simulcastStream._levels[i]._feedbackSsrc);
        ssrcsString.append(" ");
    }

    if (secondarySimulcastStream.isSet())
    {
        for (size_t i = 0; i < secondarySimulcastStream.get()._numLevels; ++i)
        {
            ssrcsString.append(secondarySimulcastStream.get()._levels[i]._ssrc);
            ssrcsString.append(",");
            ssrcsString.append(secondarySimulcastStream.get()._levels[i]._feedbackSsrc);
            ssrcsString.append(" ");
        }
    }

    utils::StringBuilder<256> ssrcWhitelistLog;
    makeSsrcWhitelistLog(ssrcWhitelist, ssrcWhitelistLog);

    logger::info("Configure video stream, endpoint id %s, ssrc %s, %s",
        _loggableId.c_str(),
        videoStream->_endpointId.c_str(),
        ssrcsString.get(),
        ssrcWhitelistLog.get());

    videoStream->_rtpMap = rtpMap;
    videoStream->_feedbackRtpMap = feedbackRtpMap;
    videoStream->_simulcastStream = simulcastStream;
    if (secondarySimulcastStream.isSet())
    {
        videoStream->_secondarySimulcastStream = secondarySimulcastStream;
    }

    videoStream->_rtpMap._absSendTimeExtId = absSendTimeExtensionId;
    if (videoStream->_rtpMap._absSendTimeExtId.isSet())
    {
        videoStream->_transport->setAbsSendTimeExtensionId(videoStream->_rtpMap._absSendTimeExtId.get());
    }
    if (videoStream->_feedbackRtpMap._format != RtpMap::Format::EMPTY)
    {
        videoStream->_transport->setVideoRtxPayloadType(videoStream->_feedbackRtpMap._payloadType);
    }

    std::memcpy(&videoStream->_ssrcWhitelist, &ssrcWhitelist, sizeof(SsrcWhitelist));
    return true;
}

bool Mixer::reconfigureVideoStream(const std::string& endpointId,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
    const SsrcWhitelist& ssrcWhitelist)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        logger::warn("Reconfigure video stream endpoint id %s, not found", _loggableId.c_str(), endpointId.c_str());
        return false;
    }
    auto videoStream = videoStreamItr->second.get();

    if (!videoStream->_isConfigured)
    {
        logger::warn("Reconfigure video stream id %s, not configured", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    utils::StringBuilder<1024> ssrcsString;
    for (size_t i = 0; i < simulcastStream._numLevels; ++i)
    {
        ssrcsString.append(simulcastStream._levels[i]._ssrc);
        ssrcsString.append(",");
        ssrcsString.append(simulcastStream._levels[i]._feedbackSsrc);
        ssrcsString.append(" ");
    }

    if (secondarySimulcastStream.isSet())
    {
        for (size_t i = 0; i < secondarySimulcastStream.get()._numLevels; ++i)
        {
            ssrcsString.append(secondarySimulcastStream.get()._levels[i]._ssrc);
            ssrcsString.append(",");
            ssrcsString.append(secondarySimulcastStream.get()._levels[i]._feedbackSsrc);
            ssrcsString.append(" ");
        }
    }

    utils::StringBuilder<256> ssrcWhitelistLog;
    makeSsrcWhitelistLog(ssrcWhitelist, ssrcWhitelistLog);

    logger::info("Reconfigure video stream id %s, endpoint id %s, ssrcs %s, %s",
        _loggableId.c_str(),
        videoStream->_id.c_str(),
        videoStream->_endpointId.c_str(),
        ssrcsString.get(),
        ssrcWhitelistLog.get());

    videoStream->_simulcastStream = simulcastStream;
    videoStream->_secondarySimulcastStream = secondarySimulcastStream;
    std::memcpy(&videoStream->_ssrcWhitelist, &ssrcWhitelist, sizeof(SsrcWhitelist));

    EngineCommand::Command command;

    if (secondarySimulcastStream.isSet())
    {
        command._type = EngineCommand::Type::ReconfigureVideoStreamSecondary;
        command._command.reconfigureVideoStreamSecondary._mixer = &_engineMixer;
        command._command.reconfigureVideoStreamSecondary._simulcastStream = videoStream->_simulcastStream;
        command._command.reconfigureVideoStreamSecondary._secondarySimulcastStream =
            videoStream->_secondarySimulcastStream.get();
        command._command.reconfigureVideoStreamSecondary._transport = videoStream->_transport.get();
        std::memcpy(&command._command.reconfigureVideoStreamSecondary._ssrcWhitelist,
            &ssrcWhitelist,
            sizeof(SsrcWhitelist));
    }
    else
    {
        command._type = EngineCommand::Type::ReconfigureVideoStream;
        command._command.reconfigureVideoStream._mixer = &_engineMixer;
        command._command.reconfigureVideoStream._simulcastStream = videoStream->_simulcastStream;
        command._command.reconfigureVideoStream._transport = videoStream->_transport.get();
        std::memcpy(&command._command.reconfigureVideoStream._ssrcWhitelist, &ssrcWhitelist, sizeof(SsrcWhitelist));
    }
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::configureDataStream(const std::string& endpointId, const uint32_t sctpPort)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end())
    {
        return false;
    }
    auto dataStream = dataStreamItr->second.get();

    dataStream->_remoteSctpPort.set(sctpPort);
    dataStream->_transport->setSctp(dataStream->_localSctpPort, dataStream->_remoteSctpPort.get());
    return true;
}

bool Mixer::configureAudioStreamTransportIce(const std::string& endpointId,
    const std::pair<std::string, std::string>& credentials,
    const ice::IceCandidates& candidates)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }

    audioStreamItr->second->_transport->setRemoteIce(credentials, candidates, _engineMixer.getAudioAllocator());
    return true;
}

bool Mixer::configureVideoStreamTransportIce(const std::string& endpointId,
    const std::pair<std::string, std::string>& credentials,
    const ice::IceCandidates& candidates)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    videoStreamItr->second->_transport->setRemoteIce(credentials, candidates, _engineMixer.getAudioAllocator());
    return true;
}

bool Mixer::configureAudioStreamTransportConnection(const std::string& endpointId, const transport::SocketAddress& peer)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }

    return audioStreamItr->second->_transport->setRemotePeer(peer);
}

bool Mixer::configureVideoStreamTransportConnection(const std::string& endpointId, const transport::SocketAddress& peer)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    return videoStreamItr->second->_transport->setRemotePeer(peer);
}

bool Mixer::configureAudioStreamTransportDtls(const std::string& endpointId,
    const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool isDtlsClient)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    audioStreamItr->second->_transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
    return true;
}

bool Mixer::configureVideoStreamTransportDtls(const std::string& endpointId,
    const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool isDtlsClient)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    videoStreamItr->second->_transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
    return true;
}

bool Mixer::configureAudioStreamTransportDisableDtls(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    audioStreamItr->second->_transport->disableDtls();
    return true;
}

bool Mixer::configureVideoStreamTransportDisableDtls(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    videoStreamItr->second->_transport->disableDtls();
    return true;
}

bool Mixer::configureBundleTransportIce(const std::string& endpointId,
    const std::pair<std::string, std::string>& credentials,
    const ice::IceCandidates& candidates)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        return false;
    }

    transportItr->second._transport->setRemoteIce(credentials, candidates, _engineMixer.getAudioAllocator());
    return true;
}

bool Mixer::configureBundleTransportDtls(const std::string& endpointId,
    const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool isDtlsClient)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        return false;
    }

    transportItr->second._transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
    return true;
}

bool Mixer::startBundleTransport(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto transportItr = _bundleTransports.find(endpointId);
    if (transportItr == _bundleTransports.end())
    {
        return false;
    }

    EngineCommand::Command command;
    command._type = EngineCommand::Type::StartTransport;
    command._command.startTransport._mixer = &_engineMixer;
    command._command.startTransport._transport = transportItr->second._transport.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::startAudioStreamTransport(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    auto audioStream = audioStreamItr->second.get();

    EngineCommand::Command command;
    command._type = EngineCommand::Type::StartTransport;
    command._command.startTransport._mixer = &_engineMixer;
    command._command.startTransport._transport = audioStream->_transport.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::startVideoStreamTransport(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }

    EngineCommand::Command command;
    command._type = EngineCommand::Type::StartTransport;
    command._command.startTransport._mixer = &_engineMixer;
    command._command.startTransport._transport = videoStreamItr->second->_transport.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::addAudioStreamToEngine(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    auto audioStream = audioStreamItr->second.get();

    logger::debug("Adding audioStream to engine, endpointId %s",
        getLoggableId().c_str(),
        audioStream->_endpointId.c_str());

    audioStream->_isConfigured = true;

    auto emplaceResult = _audioEngineStreams.emplace(audioStream->_endpointId,
        std::make_unique<EngineAudioStream>(audioStream->_endpointId,
            audioStream->_endpointIdHash,
            audioStream->_localSsrc,
            audioStream->_remoteSsrc,
            *(audioStream->_transport.get()),
            audioStream->_audioMixed,
            audioStream->_rtpMap,
            audioStream->_ssrcRewrite));

    EngineCommand::Command command;
    command._type = EngineCommand::Type::AddAudioStream;
    command._command.addAudioStream._mixer = &_engineMixer;
    command._command.addAudioStream._engineStream = emplaceResult.first->second.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::addVideoStreamToEngine(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    auto videoStream = videoStreamItr->second.get();

    logger::debug("Adding videoStream to engine, endpointId %s, %s",
        getLoggableId().c_str(),
        videoStream->_endpointId.c_str(),
        toString(videoStream->_simulcastStream._contentType));

    videoStream->_isConfigured = true;

    auto emplaceResult = _videoEngineStreams.emplace(videoStream->_endpointId,
        std::make_unique<EngineVideoStream>(videoStream->_endpointId,
            videoStream->_endpointIdHash,
            videoStream->_localSsrc,
            videoStream->_simulcastStream,
            videoStream->_secondarySimulcastStream,
            *(videoStream->_transport.get()),
            videoStream->_rtpMap,
            videoStream->_feedbackRtpMap,
            videoStream->_ssrcWhitelist,
            videoStream->_ssrcRewrite,
            _videoPinSsrcs));

    EngineCommand::Command command;
    command._type = EngineCommand::Type::AddVideoStream;
    command._command.addVideoStream._mixer = &_engineMixer;
    command._command.addVideoStream._engineStream = emplaceResult.first->second.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::addDataStreamToEngine(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end() || !dataStreamItr->second->_remoteSctpPort.isSet())
    {
        return false;
    }
    auto dataStream = dataStreamItr->second.get();

    logger::debug("Adding dataStream to engine, endpointId %s",
        getLoggableId().c_str(),
        dataStream->_endpointId.c_str());

    dataStream->_isConfigured = true;

    auto emplaceResult = _dataEngineStreams.emplace(dataStream->_endpointId,
        std::make_unique<EngineDataStream>(dataStream->_endpointId,
            dataStream->_endpointIdHash,
            *(dataStream->_transport.get()),
            _engineMixer.getSendAllocator()));

    EngineCommand::Command command;
    command._type = EngineCommand::Type::AddDataStream;
    command._command.addDataStream._mixer = &_engineMixer;
    command._command.addDataStream._engineStream = emplaceResult.first->second.get();
    _engine.pushCommand(std::move(command));

    return true;
}

bool Mixer::pinEndpoint(const size_t endpointIdHash, const std::string& pinnedEndpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto pinnedVideoStreamItr = _videoStreams.find(pinnedEndpointId);
    if (pinnedVideoStreamItr == _videoStreams.end())
    {
        return false;
    }

    EngineCommand::Command command(EngineCommand::Type::PinEndpoint);
    auto& pinEndpoint = command._command.pinEndpoint;
    pinEndpoint._mixer = &_engineMixer;
    pinEndpoint._endpointIdHash = endpointIdHash;
    pinEndpoint._pinnedEndpointIdHash = pinnedVideoStreamItr->second->_endpointIdHash;
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::unpinEndpoint(const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    EngineCommand::Command command(EngineCommand::Type::PinEndpoint);
    auto& pinEndpoint = command._command.pinEndpoint;
    pinEndpoint._mixer = &_engineMixer;
    pinEndpoint._endpointIdHash = endpointIdHash;
    pinEndpoint._pinnedEndpointIdHash = 0;
    _engine.pushCommand(std::move(command));
    return true;
}

bool Mixer::isAudioStreamGatheringComplete(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end())
    {
        return false;
    }
    return audioStreamItr->second->_transport->isGatheringComplete();
}

bool Mixer::isVideoStreamGatheringComplete(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    return videoStreamItr->second->_transport->isGatheringComplete();
}

bool Mixer::isDataStreamGatheringComplete(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end())
    {
        return false;
    }
    return dataStreamItr->second->_transport->isGatheringComplete();
}

void Mixer::sendEndpointMessage(const std::string& toEndpointId,
    const size_t fromEndpointIdHash,
    const std::string& message)
{
    assert(fromEndpointIdHash);
    if (message.size() >= EngineCommand::endpointMessageMaxSize)
    {
        return;
    }

    std::lock_guard<std::mutex> locker(_configurationLock);

    size_t toEndpointIdHash = 0;
    if (!toEndpointId.empty())
    {
        const auto dataStreamItr = _dataStreams.find(toEndpointId);
        if (dataStreamItr == _dataStreams.end())
        {
            return;
        }
        toEndpointIdHash = dataStreamItr->second->_endpointIdHash;
    }

    EngineCommand::Command command{EngineCommand::Type::EndpointMessage};
    command._command.endpointMessage._mixer = &_engineMixer;
    command._command.endpointMessage._toEndpointIdHash = toEndpointIdHash;
    command._command.endpointMessage._fromEndpointIdHash = fromEndpointIdHash;
    strncpy(command._command.endpointMessage._message, message.c_str(), EngineCommand::endpointMessageMaxSize);

    _engine.pushCommand(std::move(command));
}

RecordingStream* Mixer::findRecordingStream(const std::string& recordingId)
{
    for (auto& streamEntry : _recordingStreams)
    {
        const auto it = streamEntry.second->_attachedRecording.find(recordingId);
        if (it != streamEntry.second->_attachedRecording.end())
        {
            return streamEntry.second.get();
        }
    }
    return nullptr;
}

void Mixer::stopTransportIfNeeded(transport::RtcTransport* streamTransport, const std::string& endpointId)
{
    transport::RtcTransport* transport = nullptr;

    auto bundleTransportItr = _bundleTransports.find(endpointId);
    if (bundleTransportItr != _bundleTransports.end())
    {
        if (_audioStreams.find(endpointId) == _audioStreams.end() &&
            _videoStreams.find(endpointId) == _videoStreams.end() &&
            _dataStreams.find(endpointId) == _dataStreams.end())
        {
            logger::info("EngineStream removed, endpointId %s. Has bundle transport %s but no other related streams.",
                _loggableId.c_str(),
                endpointId.c_str(),
                bundleTransportItr->second._transport->getLoggableId().c_str());

            transport = bundleTransportItr->second._transport.get();
        }
    }
    else
    {
        logger::info("EngineStream removed, endpointId %s. No bundle transport.",
            _loggableId.c_str(),
            endpointId.c_str());

        transport = streamTransport;
    }

    if (transport == nullptr)
    {
        return;
    }

    logTransportPacketLoss(endpointId, *transport, _loggableId.c_str());

    logger::info("Engine stream removed, stopping transport %s, endpointId %s.",
        _loggableId.c_str(),
        transport->getLoggableId().c_str(),
        endpointId.c_str());

    // Allow pending transmissions to complete
    transport->stop();
    if (!waitForPendingJobs(700, 5, *transport))
    {
        logger::error("Transport for endpointId %s did not finish pending jobs in time. count=%u. Continuing "
                      "deletion anyway.",
            _loggableId.c_str(),
            endpointId.c_str(),
            transport->getJobCounter().load());
    }
}

bool Mixer::addOrUpdateRecording(const std::string& conferenceId,
    const std::vector<api::RecordingChannel>& channels,
    const RecordingDescription& recordingDescription)
{
    if (!(recordingDescription._isAudioEnabled || recordingDescription._isVideoEnabled))
    {
        logger::error("Received a recording description without any media enabled. RecordingId %s.",
            _loggableId.c_str(),
            recordingDescription._recordingId.c_str());

        return false;
    }

    std::lock_guard<std::mutex> locker(_configurationLock);

    RecordingStream* stream = findRecordingStream(recordingDescription._recordingId);
    if (stream)
    {
        const bool wasAudioEnabled = stream->_audioActiveRecCount > 0;
        const bool wasVideoEnabled = stream->_videoActiveRecCount > 0;
        const bool wasScreenSharingEnabled = stream->_screenSharingActiveRecCount > 0;

        auto attachedRecordingDescription = stream->_attachedRecording.at(recordingDescription._recordingId);
        if (attachedRecordingDescription._isAudioEnabled != recordingDescription._isAudioEnabled)
        {
            attachedRecordingDescription._isAudioEnabled = recordingDescription._isAudioEnabled;
            recordingDescription._isAudioEnabled ? ++stream->_audioActiveRecCount : --stream->_audioActiveRecCount;
        }
        if (attachedRecordingDescription._isVideoEnabled != recordingDescription._isVideoEnabled)
        {
            attachedRecordingDescription._isVideoEnabled = recordingDescription._isVideoEnabled;
            recordingDescription._isVideoEnabled ? ++stream->_videoActiveRecCount : --stream->_videoActiveRecCount;
        }
        if (attachedRecordingDescription._isScreenSharingEnabled != recordingDescription._isScreenSharingEnabled)
        {
            attachedRecordingDescription._isScreenSharingEnabled = recordingDescription._isScreenSharingEnabled;
            recordingDescription._isScreenSharingEnabled ? ++stream->_screenSharingActiveRecCount
                                                         : --stream->_screenSharingActiveRecCount;
        }
        addRecordingTransportsToRecordingStream(stream, channels);
        updateRecordingEngineStreamModalities(*stream, wasAudioEnabled, wasVideoEnabled, wasScreenSharingEnabled);
    }
    else
    {
        auto streamEntry = _recordingStreams.find(conferenceId);
        if (streamEntry == _recordingStreams.end())
        {
            streamEntry =
                _recordingStreams.emplace(conferenceId, std::make_unique<RecordingStream>(conferenceId)).first;
        }

        stream = streamEntry->second.get();
        addRecordingTransportsToRecordingStream(stream, channels);

        const bool wasAudioEnabled = stream->_audioActiveRecCount > 0;
        const bool wasVideoEnabled = stream->_videoActiveRecCount > 0;
        const bool wasScreenSharingEnabled = stream->_screenSharingActiveRecCount > 0;

        const auto recordingEmplaced =
            stream->_attachedRecording.emplace(recordingDescription._recordingId, recordingDescription);

        stream->_audioActiveRecCount += recordingDescription._isAudioEnabled ? 1 : 0;
        stream->_videoActiveRecCount += recordingDescription._isVideoEnabled ? 1 : 0;
        stream->_screenSharingActiveRecCount += recordingDescription._isScreenSharingEnabled ? 1 : 0;

        const bool isAudioEnabled = stream->_audioActiveRecCount > 0;
        const bool isVideoEnabled = stream->_videoActiveRecCount > 0;
        const bool isScreenSharingEnabled = stream->_screenSharingActiveRecCount > 0;

        if (stream->_attachedRecording.size() == 1)
        {
            auto recEventPacketCache = std::make_unique<PacketCache>("RecordingEventPacketCache");
            auto emplaceResult = _recordingEngineStreams.emplace(conferenceId,
                std::make_unique<EngineRecordingStream>(stream->_id,
                    stream->_endpointIdHash,
                    isAudioEnabled,
                    isVideoEnabled,
                    isScreenSharingEnabled,
                    *recEventPacketCache));
            _recordingEventPacketCache.emplace(stream->_endpointIdHash, std::move(recEventPacketCache));

            logger::info("New recording stream, recording stream %s. a: %c v: %c ss: %c",
                _loggableId.c_str(),
                stream->_id.c_str(),
                isAudioEnabled ? 'e' : 'd',
                isVideoEnabled ? 'e' : 'd',
                isScreenSharingEnabled ? 'e' : 'd');

            EngineCommand::Command command{EngineCommand::Type::AddRecordingStream};
            command._command.addRecordingStream._mixer = &_engineMixer;
            command._command.addRecordingStream._recordingStream = emplaceResult.first->second.get();
            _engine.pushCommand(std::move(command));
        }
        else
        {
            updateRecordingEngineStreamModalities(*stream, wasAudioEnabled, wasVideoEnabled, wasScreenSharingEnabled);
        }

        auto engineStream = _recordingEngineStreams.at(conferenceId).get();
        for (const auto& transport : stream->_transports)
        {
            EngineCommand::Command command{EngineCommand::Type::AddTransportToRecordingStream};
            command._command.addTransportToRecordingStream._mixer = &_engineMixer;
            command._command.addTransportToRecordingStream._streamIdHash = engineStream->_endpointIdHash;
            command._command.addTransportToRecordingStream._transport = transport.second.get();
            command._command.addTransportToRecordingStream._recUnackedPacketsTracker =
                stream->_recEventUnackedPacketsTracker[transport.first].get();
            _engine.pushCommand(std::move(command));
        }

        EngineCommand::Command command{EngineCommand::Type::StartRecording};
        command._command.startRecording._mixer = &_engineMixer;
        command._command.startRecording._recordingStream = engineStream;
        command._command.startRecording._recordingDesc = &recordingEmplaced.first->second;
        _engine.pushCommand(std::move(command));

        return true;
    }

    return false;
}

void Mixer::updateRecordingEngineStreamModalities(const RecordingStream& recordingStream,
    const bool wasAudioEnabled,
    const bool wasVideoEnabled,
    const bool wasScreenSharingEnabled)
{
    const bool isAudioEnabled = recordingStream._audioActiveRecCount > 0;
    const bool isVideoEnabled = recordingStream._videoActiveRecCount > 0;
    const bool isScreenSharingEnabled = recordingStream._screenSharingActiveRecCount > 0;

    if (!(wasAudioEnabled == isAudioEnabled && isVideoEnabled == wasVideoEnabled &&
            isScreenSharingEnabled == wasScreenSharingEnabled))
    {
        auto* engineStream = _recordingEngineStreams.at(recordingStream._id).get();

        logger::info("Update recording modalities, recording stream %s. a: %c -> %c .v: %c -> %c. ss: %c -> %c",
            _loggableId.c_str(),
            recordingStream._id.c_str(),
            wasAudioEnabled ? 'e' : 'd',
            isAudioEnabled ? 'e' : 'd',
            wasVideoEnabled ? 'e' : 'd',
            isVideoEnabled ? 'e' : 'd',
            wasScreenSharingEnabled ? 'e' : 'd',
            isScreenSharingEnabled ? 'e' : 'd');

        EngineCommand::Command command{EngineCommand::Type::UpdateRecordingStreamModalities};
        command._command.updateRecordingStreamModalities._mixer = &_engineMixer;
        command._command.updateRecordingStreamModalities._recordingStream = engineStream;
        command._command.updateRecordingStreamModalities._isAudioEnabled = isAudioEnabled;
        command._command.updateRecordingStreamModalities._isVideoEnabled = isVideoEnabled;
        command._command.updateRecordingStreamModalities._isScreenSharingEnabled = isScreenSharingEnabled;
        _engine.pushCommand(std::move(command));
    }
}

void Mixer::addRecordingTransportsToRecordingStream(RecordingStream* recordingStream,
    const std::vector<api::RecordingChannel>& channels)
{
    for (const auto& channel : channels)
    {
        auto endpointIdHash = std::hash<std::string>{}(channel._id);
        const auto transportEntry = recordingStream->_transports.find(endpointIdHash);
        if (transportEntry == recordingStream->_transports.end())
        {
            auto transport = _transportFactory.createForRecording(endpointIdHash,
                recordingStream->_endpointIdHash,
                transport::SocketAddress::parse(channel._host, channel._port),
                channel._aesKey,
                channel._aesSalt);

            if (transport)
            {
                EngineCommand::Command command{EngineCommand::Type::StartRecordingTransport};
                command._command.startRecordingTransport._mixer = &_engineMixer;
                command._command.startRecordingTransport._transport = transport.get();

                recordingStream->_transports.emplace(endpointIdHash, std::move(transport));
                recordingStream->_recEventUnackedPacketsTracker.emplace(endpointIdHash,
                    std::make_unique<UnackedPacketsTracker>("RecEventUnackedPacketsTracker", 50));

                _engine.pushCommand(std::move(command));
            }
            else
            {
                logger::error("Creation of recording transport has failed for channel %s:%u",
                    _loggableId.c_str(),
                    channel._host.c_str(),
                    channel._port);
            }
        }
    }
}

bool Mixer::removeRecordingTransports(const std::string& conferenceId,
    const std::vector<api::RecordingChannel>& channels)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto streamEntry = _recordingStreams.find(conferenceId);
    if (streamEntry != _recordingStreams.end())
    {
        const auto stream = streamEntry->second.get();
        if (stream->_markedForDeletion)
        {
            return false;
        }

        auto engineStreamEntry = _recordingEngineStreams.find(stream->_id);
        if (engineStreamEntry == _recordingEngineStreams.end())
        {
            return false;
        }

        for (const auto& channel : channels)
        {
            auto endpointIdHash = std::hash<std::string>{}(channel._id);
            auto transportItr = stream->_transports.find(endpointIdHash);
            if (transportItr == stream->_transports.end())
            {
                continue;
            }

            EngineCommand::Command command{EngineCommand::Type::RemoveTransportFromRecordingStream};
            command._command.removeTransportFromRecordingStream._mixer = &_engineMixer;
            command._command.removeTransportFromRecordingStream._streamIdHash =
                engineStreamEntry->second->_endpointIdHash;
            command._command.removeTransportFromRecordingStream._endpointIdHash = transportItr->first;
            _engine.pushCommand(std::move(command));
        }

        if (stream->_transports.empty())
        {
            stream->_markedForDeletion = true;
            EngineCommand::Command removeRecStreamCommand{EngineCommand::Type::RemoveRecordingStream};
            removeRecStreamCommand._command.removeRecordingStream._mixer = &_engineMixer;
            removeRecStreamCommand._command.removeRecordingStream._recordingStream = engineStreamEntry->second.get();
            _engine.pushCommand(std::move(removeRecStreamCommand));
        }
        return true;
    }

    return false;
}

bool Mixer::removeRecording(const std::string& recordingId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    RecordingStream* stream = findRecordingStream(recordingId);
    if (stream)
    {
        if (stream->_markedForDeletion)
        {
            return false;
        }

        auto recordingDescriptionEntry = stream->_attachedRecording.find(recordingId);
        if (recordingDescriptionEntry == stream->_attachedRecording.end())
        {
            return false;
        }
        const bool wasAudioEnabled = stream->_audioActiveRecCount > 0;
        const bool wasVideoEnabled = stream->_videoActiveRecCount > 0;
        const bool wasScreenSharingEnabled = stream->_screenSharingActiveRecCount > 0;

        stream->_audioActiveRecCount -= recordingDescriptionEntry->second._isAudioEnabled ? 1 : 0;
        stream->_videoActiveRecCount -= recordingDescriptionEntry->second._isVideoEnabled ? 1 : 0;
        stream->_screenSharingActiveRecCount -= recordingDescriptionEntry->second._isScreenSharingEnabled ? 1 : 0;

        auto engineStreamEntry = _recordingEngineStreams.find(stream->_id);
        if (engineStreamEntry == _recordingEngineStreams.end())
        {
            return false;
        }

        EngineCommand::Command stopRecCommand{EngineCommand::Type::StopRecording};
        stopRecCommand._command.stopRecording._mixer = &_engineMixer;
        stopRecCommand._command.stopRecording._recordingStream = engineStreamEntry->second.get();
        stopRecCommand._command.stopRecording._recordingDesc = &recordingDescriptionEntry->second;
        _engine.pushCommand(std::move(stopRecCommand));

        if (stream->_attachedRecording.size() == 1)
        {
            stream->_markedForDeletion = true;
            EngineCommand::Command removeRecStreamCommand{EngineCommand::Type::RemoveRecordingStream};
            removeRecStreamCommand._command.removeRecordingStream._mixer = &_engineMixer;
            removeRecStreamCommand._command.removeRecordingStream._recordingStream = engineStreamEntry->second.get();
            _engine.pushCommand(std::move(removeRecStreamCommand));
        }
        else
        {
            updateRecordingEngineStreamModalities(*stream, wasAudioEnabled, wasVideoEnabled, wasScreenSharingEnabled);
        }
    }
    return true;
}

void Mixer::engineRecordingDescStopped(const RecordingDescription& recordingDesc)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    RecordingStream* stream = findRecordingStream(recordingDesc._recordingId);
    if (!stream)
    {
        return;
    }

    const size_t erasedCount = stream->_attachedRecording.erase(recordingDesc._recordingId);
    assert(erasedCount == 1);
}

void Mixer::engineRecordingStreamRemoved(EngineRecordingStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto streamItr = _recordingStreams.find(engineStream->_id);
    assert(streamItr != _recordingStreams.end());
    auto& stream = streamItr->second;
    assert(stream->_attachedRecording.empty());

    for (const auto& transportEntry : stream->_transports)
    {
        logger::info("RecordingStream id %s, endpointId %s deleted.",
            _loggableId.c_str(),
            stream->_id.c_str(),
            transportEntry.second->getRemotePeer().toString().c_str());

        // Try first wait for pending jobs without stop the transport
        // as we may have some recording events to be sent and we don't
        // want to lose them
        waitForPendingJobs(200, 5, *transportEntry.second);
        transportEntry.second->stop();
        _engineMixer.getJobManager().abortTimedJobs(transportEntry.second->getId());

        if (!waitForPendingJobs(500, 5, *transportEntry.second))
        {
            logger::error("RecordingStream id %s did not finish pending jobs in time. count=%u. Continuing "
                          "deletion anyway.",
                _loggableId.c_str(),
                stream->_id.c_str(),
                transportEntry.second->getJobCounter().load());
        }
    }

    _recordingEventPacketCache.erase(stream->_endpointIdHash);
    _recordingRtpPacketCaches.erase(stream->_endpointIdHash);
    _recordingStreams.erase(streamItr);
    _recordingEngineStreams.erase(engineStream->_id);
}

void Mixer::allocateRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto& recordingRtpPacketCaches = _recordingRtpPacketCaches[endpointIdHash];
    auto findResult = recordingRtpPacketCaches.find(ssrc);
    if (findResult != recordingRtpPacketCaches.cend())
    {
        return;
    }

    logger::info("Allocating RecordingPacketCache for ssrc %u", _loggableId.c_str(), ssrc);

    auto packetCache = std::make_unique<PacketCache>("RecordingRtpPacketCache", ssrc);
    {
        EngineCommand::Command command(EngineCommand::Type::AddRecordingRtpPacketCache);
        command._command.addRecordingRtpPacketCache._mixer = &_engineMixer;
        command._command.addRecordingRtpPacketCache._ssrc = ssrc;
        command._command.addRecordingRtpPacketCache._endpointIdHash = endpointIdHash;
        command._command.addRecordingRtpPacketCache._packetCache = packetCache.get();
        _engine.pushCommand(std::move(command));
    }
    recordingRtpPacketCaches.emplace(ssrc, std::move(packetCache));
}

void Mixer::freeRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto& recordingPacketCaches = _recordingRtpPacketCaches[endpointIdHash];
    auto findResult = recordingPacketCaches.find(ssrc);
    if (findResult == recordingPacketCaches.cend())
    {
        return;
    }

    logger::info("Freeing RecordingRtpPacketCache for ssrc %u", _loggableId.c_str(), ssrc);
    recordingPacketCaches.erase(ssrc);
}

void Mixer::removeRecordingTransport(const std::string& streamId, const size_t endpointIdHash)
{

    std::lock_guard<std::mutex> locker(_configurationLock);

    auto streamItr = _recordingStreams.find(streamId);
    if (streamItr == _recordingStreams.end())
    {
        return;
    }

    auto& stream = streamItr->second;

    auto transportItr = stream->_transports.find(endpointIdHash);
    if (transportItr == stream->_transports.end())
    {
        return;
    }

    // Try first wait for pending jobs without stop the transport
    // as we may have some recording events to be sent and we don't
    // want to lose them
    waitForPendingJobs(200, 5, *transportItr->second);
    transportItr->second->stop();
    _engineMixer.getJobManager().abortTimedJobs(transportItr->second->getId());
    if (!waitForPendingJobs(500, 5, *transportItr->second))
    {
        logger::error("RecordingTransport for streamId %s did not finish pending jobs in time. count=%u. Continuing "
                      "deletion anyway.",
            _loggableId.c_str(),
            streamId.c_str(),
            transportItr->second->getJobCounter().load());
    }

    stream->_recEventUnackedPacketsTracker.erase(endpointIdHash);
    stream->_transports.erase(endpointIdHash);
}

} // namespace bridge
