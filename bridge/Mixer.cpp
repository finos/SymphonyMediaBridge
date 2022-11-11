#include "bridge/Mixer.h"
#include "api/ConferenceEndpoint.h"
#include "api/RecordingChannel.h"
#include "bridge/AudioStream.h"
#include "bridge/AudioStreamDescription.h"
#include "bridge/Barbell.h"
#include "bridge/BarbellVideoStreamDescription.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/TransportDescription.h"
#include "bridge/VideoStreamDescription.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/PacketCache.h"
#include "config/Config.h"
#include "jobmanager/JobManager.h"
#include "logger/Logger.h"
#include "transport/TransportFactory.h"
#include "transport/ice/IceCandidate.h"
#include "utils/IdGenerator.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include "utils/StdExtensions.h"
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

        logger::info("EndpointId %s %s, outbound ssrc %u, packets %u"
                     ", last sent seq %u, last received %u, reported loss count %u, initial RTP timestamp %u, RTP "
                     "timestamp %u, brutto octets sent: %" PRIu64,
            mixerId,
            endpointId.c_str(),
            transport.getLoggableId().c_str(),
            ssrc,
            reportSummary.packetsSent,
            reportSummary.sequenceNumberSent,
            reportSummary.extendedSeqNoReceived,
            reportSummary.lostPackets,
            reportSummary.initialRtpTimestamp,
            reportSummary.rtpTimestamp,
            reportSummary.octets);
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
    if (!ssrcWhitelist.enabled)
    {
        outLogString.append("disabled");
        return;
    }

    outLogString.append("enabled ");
    outLogString.append(ssrcWhitelist.numSsrcs);

    switch (ssrcWhitelist.numSsrcs)
    {
    case 1:
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist.ssrcs[0]);
        break;

    case 2:
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist.ssrcs[0]);
        outLogString.append(" ");
        outLogString.append(ssrcWhitelist.ssrcs[1]);
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
    size_t logInstanceId,
    transport::TransportFactory& transportFactory,
    Engine& engine,
    std::unique_ptr<EngineMixer> engineMixer,
    utils::IdGenerator& idGenerator,
    utils::SsrcGenerator& ssrcGenerator,
    const config::Config& config,
    const std::vector<uint32_t>& audioSsrcs,
    const std::vector<api::SimulcastGroup>& videoSsrcs,
    const std::vector<api::SsrcPair>& videoPinSsrcs,
    bool useGlobalPort)
    : _config(config),
      _id(std::move(id)),
      _loggableId("Mixer", logInstanceId),
      _markedForDeletion(false),
      _audioSsrcs(audioSsrcs),
      _videoSsrcs(videoSsrcs),
      _videoPinSsrcs(videoPinSsrcs),
      _transportFactory(transportFactory),
      _engine(engine),
      _engineMixer(std::move(engineMixer)),
      _idGenerator(idGenerator),
      _ssrcGenerator(ssrcGenerator),
      _useGlobalPort(useGlobalPort)
{
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
    }

    for (auto& audioStreamEntry : _audioStreams)
    {
        assert(audioStreamEntry.second->transport);
        if (_bundleTransports.find(audioStreamEntry.second->endpointId) != _bundleTransports.end())
        {
            continue;
        }

        logTransportPacketLoss("", *audioStreamEntry.second->transport, _loggableId.c_str());
        audioStreamEntry.second->transport->stop();
    }

    for (auto& videoStreamEntry : _videoStreams)
    {
        assert(videoStreamEntry.second->transport);
        if (_bundleTransports.find(videoStreamEntry.second->endpointId) != _bundleTransports.end())
        {
            continue;
        }

        logTransportPacketLoss("", *videoStreamEntry.second->transport, _loggableId.c_str());
        videoStreamEntry.second->transport->stop();
    }

    for (auto& barbell : _barbells)
    {
        logTransportPacketLoss(barbell.second->id, *barbell.second->transport, _loggableId.c_str());
        barbell.second->transport->stop();
    }

    _barbellPorts.clear();
    _rtpPorts.clear();
}

bool Mixer::hasPendingTransportJobs()
{
    for (auto& bundle : _bundleTransports)
    {
        if (bundle.second._transport->hasPendingJobs())
        {
            return true;
        }
    }

    for (auto& audioStream : _audioStreams)
    {
        if (audioStream.second->transport && audioStream.second->transport->hasPendingJobs())
        {
            return true;
        }
    }

    for (auto& videoStream : _videoStreams)
    {
        if (videoStream.second->transport && videoStream.second->transport->hasPendingJobs())
        {
            return true;
        }
    }

    for (auto& barbell : _barbells)
    {
        if (barbell.second->transport->hasPendingJobs())
        {
            return true;
        }
    }

    _engineMixer->flush();

    return false;
}

bool Mixer::addBundleTransportIfNeeded(const std::string& endpointId, const ice::IceRole iceRole)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_bundleTransports.find(endpointId) != _bundleTransports.end())
    {
        return true;
    }

    if (!_useGlobalPort && _rtpPorts.empty())
    {
        if (!_transportFactory.openRtpMuxPorts(_rtpPorts, 1024))
        {
            logger::error("Failed to open isolated port for this conference, endpointId %s",
                _loggableId.c_str(),
                endpointId.c_str());
            return false;
        }
    }

    const auto endpointIdHash = utils::hash<std::string>{}(endpointId);
    auto transport = _useGlobalPort
        ? _transportFactory.create(iceRole, 512, endpointIdHash)
        : _transportFactory.createOnPorts(iceRole, 512, endpointIdHash, _rtpPorts, 16, 256, true, true);

    const auto emplaceResult = _bundleTransports.emplace(endpointId, transport);
    if (!emplaceResult.second)
    {
        logger::error("Failed to create bundle transport, endpointId %s", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    logger::info("Created bundle transport, endpointId %s, endpointIdHash %" PRIu64
                 ", transport %s (%p), ice controlling %c",
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
    bool rewriteSsrcs,
    bool isDtlsEnabled,
    utils::Optional<uint32_t> idleTimeoutSeconds)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_audioStreams.find(endpointId) != _audioStreams.end())
    {
        logger::warn("AudioStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());
    auto transport = iceRole.isSet()
        ? _transportFactory.create(iceRole.get(), 32, utils::hash<std::string>{}(endpointId))
        : _transportFactory.create(32, utils::hash<std::string>{}(endpointId));

    if (!transport)
    {
        logger::error("Failed to create transport for AudioStream with endpointId %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    const auto streamItr = _audioStreams.emplace(endpointId,
        std::make_unique<AudioStream>(outId,
            endpointId,
            _ssrcGenerator.next(),
            transport,
            audioMixed,
            rewriteSsrcs,
            isDtlsEnabled,
            idleTimeoutSeconds));

    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created audioStream id %s, endpointId %s, endpointIdHash %zu, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->endpointIdHash,
        streamItr.first->second->transport->getLoggableId().c_str());

    return streamItr.first->second->transport->isInitialized();
}

void Mixer::allocateAudioBuffer(uint32_t ssrc)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto findResult = _audioBuffers.find(ssrc);
    if (findResult != _audioBuffers.cend())
    {
        return;
    }

    logger::info("Allocating audio buffer for ssrc %u", getLoggableId().c_str(), ssrc);

    auto audioBuffer = std::make_unique<EngineMixer::AudioBuffer>();
    auto* rawAudioBuffer = audioBuffer.get();
    _audioBuffers.emplace(ssrc, std::move(audioBuffer));
    _engine.post(utils::bind(&EngineMixer::addAudioBuffer, _engineMixer.get(), ssrc, rawAudioBuffer));
}

bool Mixer::addVideoStream(std::string& outId,
    const std::string& endpointId,
    const utils::Optional<ice::IceRole>& iceRole,
    bool rewriteSsrcs,
    bool isDtlsEnabled,
    utils::Optional<uint32_t> idleTimeoutSeconds)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    if (_videoStreams.find(endpointId) != _videoStreams.end())
    {
        logger::warn("VideoStream with endpointId %s already exists", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    outId = std::to_string(_idGenerator.next());
    auto transport = iceRole.isSet()
        ? _transportFactory.create(iceRole.get(), 32, utils::hash<std::string>{}(endpointId))
        : _transportFactory.create(32, utils::hash<std::string>{}(endpointId));

    if (!transport)
    {
        logger::error("Failed to create transport for VideoStream with endpointId %s",
            _loggableId.c_str(),
            endpointId.c_str());
        return false;
    }

    const auto emplaceResult = _videoStreams.emplace(endpointId,
        std::make_unique<VideoStream>(outId,
            endpointId,
            _ssrcGenerator.next(),
            transport,
            rewriteSsrcs,
            isDtlsEnabled,
            idleTimeoutSeconds));

    if (!emplaceResult.second)
    {
        return false;
    }

    logger::info("Created videoStream id %s, endpointId %s, endpointIdHash %zu, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        emplaceResult.first->second->endpointIdHash,
        emplaceResult.first->second->transport->getLoggableId().c_str());

    return emplaceResult.first->second->transport->isInitialized();
}

bool Mixer::addBundledAudioStream(std::string& outId,
    const std::string& endpointId,
    const bool audioMixed,
    const bool ssrcRewrite,
    utils::Optional<uint32_t> idleTimeoutSeconds)
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
            ssrcRewrite,
            true,
            idleTimeoutSeconds));

    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created bundled audioStream id %s, endpointId %s, endpointIdHash %zu, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->endpointIdHash,
        streamItr.first->second->transport->getLoggableId().c_str());

    return streamItr.first->second->transport->isInitialized();
}

bool Mixer::addBundledVideoStream(std::string& outId,
    const std::string& endpointId,
    const bool ssrcRewrite,
    utils::Optional<uint32_t> idleTimeoutSeconds)
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
            ssrcRewrite,
            true,
            idleTimeoutSeconds));

    if (!streamItr.second)
    {
        return false;
    }

    logger::info("Created bundled videoStream id %s, endpointId %s, endpointIdHash %zu, transport %s",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->endpointIdHash,
        streamItr.first->second->transport->getLoggableId().c_str());

    return streamItr.first->second->transport->isInitialized();
}

bool Mixer::addBundledDataStream(std::string& outId,
    const std::string& endpointId,
    utils::Optional<uint32_t> idleTimeoutSeconds)
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
        std::make_unique<DataStream>(outId, endpointId, transportItr->second._transport, idleTimeoutSeconds));
    if (!streamItr.second)
    {
        return false;
    }
    if (_config.sctp.fixedPort)
    {
        streamItr.first->second->localSctpPort = 5000;
    }

    logger::info("Created bundled dataStream id %s, endpointId %s, transport %s, port %u",
        _loggableId.c_str(),
        outId.c_str(),
        endpointId.c_str(),
        streamItr.first->second->transport->getLoggableId().c_str(),
        streamItr.first->second->localSctpPort);

    return streamItr.first->second->transport->isInitialized();
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

    if (audioStream->markedForDeletion)
    {
        return true;
    }

    audioStream->markedForDeletion = true;
    auto engineStreamItr = _audioEngineStreams.find(audioStream->endpointId);
    if (engineStreamItr == _audioEngineStreams.end())
    {
        return true;
    }

    _engine.post(utils::bind(static_cast<void (EngineMixer::*)(EngineAudioStream*)>(&EngineMixer::removeStream),
        _engineMixer.get(),
        engineStreamItr->second.get()));
    return true;
}

bool Mixer::removeAudioStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto audioStreamItr = std::find_if(_audioStreams.begin(), _audioStreams.end(), [&](const auto& element) {
            return element.second->id.compare(id) == 0;
        });

        if (audioStreamItr == _audioStreams.end())
        {
            return false;
        }

        endpointId = audioStreamItr->second->endpointId;
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

    if (videoStream->markedForDeletion)
    {
        return true;
    }

    videoStream->markedForDeletion = true;
    auto engineStreamItr = _videoEngineStreams.find(videoStream->endpointId);
    if (engineStreamItr == _videoEngineStreams.end())
    {
        return true;
    }

    _engine.post(utils::bind(static_cast<void (EngineMixer::*)(EngineVideoStream*)>(&EngineMixer::removeStream),
        _engineMixer.get(),
        engineStreamItr->second.get()));

    return true;
}

bool Mixer::removeVideoStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto videoStreamItr = std::find_if(_videoStreams.begin(), _videoStreams.end(), [&](const auto& element) {
            return element.second->id.compare(id) == 0;
        });

        if (videoStreamItr == _videoStreams.end())
        {
            return false;
        }

        endpointId = videoStreamItr->second->endpointId;
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

    if (dataStream->markedForDeletion)
    {
        return true;
    }

    dataStream->markedForDeletion = true;
    auto engineStreamItr = _dataEngineStreams.find(dataStream->endpointId);
    if (engineStreamItr == _dataEngineStreams.end())
    {
        return true;
    }

    _engine.post(utils::bind(static_cast<void (EngineMixer::*)(EngineDataStream*)>(&EngineMixer::removeStream),
        _engineMixer.get(),
        engineStreamItr->second.get()));
    return true;
}

bool Mixer::removeDataStreamId(const std::string& id)
{
    std::string endpointId;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto dataStreamItr = std::find_if(_dataStreams.begin(), _dataStreams.end(), [&](const auto& element) {
            return element.second->id.compare(id) == 0;
        });

        if (dataStreamItr == _dataStreams.end())
        {
            return false;
        }

        endpointId = dataStreamItr->second->endpointId;
    }

    return removeDataStream(endpointId);
}

void Mixer::engineAudioStreamRemoved(EngineAudioStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->endpointId;
    auto streamItr = _audioStreams.find(endpointId);
    if (streamItr == _audioStreams.end())
    {
        stopTransportIfNeeded(&engineStream->transport, endpointId);
        _audioEngineStreams.erase(endpointId);

        logger::warn("EngineAudioStream endpointId %s removed, no matching audioStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    if (engineStream->remoteSsrc.isSet() && _audioBuffers.find(engineStream->remoteSsrc.get()) != _audioBuffers.end())
    {
        _audioBuffers.erase(engineStream->remoteSsrc.get());
    }

    auto& stream = streamItr->second;
    logger::info("AudioStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->transport;

    _audioStreams.erase(endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _audioEngineStreams.erase(endpointId);
}

void Mixer::engineVideoStreamRemoved(EngineVideoStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->endpointId;
    auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.end())
    {
        stopTransportIfNeeded(&engineStream->transport, endpointId);
        _videoEngineStreams.erase(endpointId);

        logger::warn("EngineVideoStream endpointId %s removed, no matching videoStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    auto& stream = streamItr->second;
    logger::info("VideoStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->transport;
    _videoStreams.erase(stream->endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _videoEngineStreams.erase(endpointId);
}

void Mixer::engineDataStreamRemoved(EngineDataStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    const auto endpointId = engineStream->endpointId;
    auto streamItr = _dataStreams.find(endpointId);
    if (streamItr == _dataStreams.end())
    {
        stopTransportIfNeeded(&engineStream->transport, endpointId);
        _dataEngineStreams.erase(endpointId);

        logger::warn("EngineDataStream endpointId %s removed, no matching dataStream found",
            _loggableId.c_str(),
            endpointId.c_str());
        return;
    }

    auto& stream = streamItr->second;

    logger::info("DataStream id %s, endpointId %s deleted.",
        _loggableId.c_str(),
        stream->id.c_str(),
        endpointId.c_str());

    auto streamTransport = stream->transport;
    _dataStreams.erase(stream->endpointId);

    stopTransportIfNeeded(streamTransport.get(), endpointId);
    _dataEngineStreams.erase(endpointId);
}

std::unordered_set<std::string> Mixer::getEndpoints() const
{
    std::unordered_set<std::string> endpoints;
    for (const auto& it : _audioStreams)
    {
        endpoints.insert(it.first);
    }
    for (const auto& it : _videoStreams)
    {
        endpoints.insert(it.first);
    }
    return endpoints;
}

std::map<size_t, ActiveTalker> Mixer::getActiveTalkers()
{
    return _engineMixer->getActiveTalkers();
}

bool Mixer::getEndpointInfo(const std::string& endpointId,
    api::ConferenceEndpoint& endpoint,
    const std::map<size_t, ActiveTalker>& activeTalkers)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto audio = _audioStreams.find(endpointId);
    endpoint.id = endpointId;
    endpoint.isDominantSpeaker = false;
    endpoint.isActiveTalker = false;
    bool foundAudio = false;
    if (audio != _audioStreams.cend())
    {
        if (audio->second)
        {
            foundAudio = true;
            endpoint.isDominantSpeaker = audio->second->endpointIdHash == _engineMixer->getDominantSpeakerId();
            auto transport = audio->second->transport;
            endpoint.iceState = transport->getIceState();
            endpoint.dtlsState = transport->getDtlsState();

            auto const& it = activeTalkers.find(audio->second->endpointIdHash);
            endpoint.isActiveTalker = it != activeTalkers.end();

            if (it != activeTalkers.end())
            {
                assert(endpoint.isActiveTalker == true);
                endpoint.activeTalkerInfo.endpointHashId = it->second.endpointHashId;
                endpoint.activeTalkerInfo.isPtt = it->second.isPtt;
                endpoint.activeTalkerInfo.score = it->second.score;
                endpoint.activeTalkerInfo.noiseLevel = it->second.noiseLevel;
            }
        }
    }

    return foundAudio || _videoStreams.find(endpointId) != _videoStreams.cend();
}

bool Mixer::getEndpointExtendedInfo(const std::string& endpointId,
    api::ConferenceEndpointExtendedInfo& endpoint,
    const std::map<size_t, ActiveTalker>& activeTalkers)
{
    if (!getEndpointInfo(endpointId, endpoint.basicEndpointInfo, activeTalkers))
    {
        return false;
    }

    const auto audio = _audioStreams.find(endpointId);
    const auto transport = audio->second->transport;
    const auto& remoteSsrc = audio->second->remoteSsrc;
    if (remoteSsrc.isSet())
    {
        endpoint.userId = _engineMixer->getC9UserId(remoteSsrc.get());
        endpoint.ssrcOriginal = remoteSsrc.get();
        endpoint.ssrcRewritten = audio->second->localSsrc;
    }

    auto remote = transport->getRemotePeer();
    endpoint.remotePort = remote.getPort();
    endpoint.remoteIP = remote.ipToString();
    endpoint.localIP = transport->getLocalRtpPort().ipToString();
    endpoint.localPort = transport->getLocalRtpPort().getPort();
    auto transportType = transport->getSelectedTransportType();
    endpoint.protocol = (transportType.isSet() ? ice::toString(transportType.get()) : "n/a");

    return true;
}

bool Mixer::getAudioStreamDescription(const std::string& endpointId, AudioStreamDescription& outDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _audioStreams.find(endpointId);
    if (streamItr == _audioStreams.cend())
    {
        return false;
    }

    outDescription = AudioStreamDescription(*streamItr->second);
    if (streamItr->second->ssrcRewrite)
    {
        for (auto ssrc : _audioSsrcs)
        {
            outDescription.ssrcs.push_back(ssrc);
        }
    }
    return true;
}

void Mixer::getAudioStreamDescription(AudioStreamDescription& outDescription)
{
    outDescription = AudioStreamDescription();
    for (auto ssrc : _audioSsrcs)
    {
        outDescription.ssrcs.push_back(ssrc);
    }
}

bool Mixer::getVideoStreamDescription(const std::string& endpointId, VideoStreamDescription& outDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.cend())
    {
        return false;
    }

    outDescription = VideoStreamDescription(*streamItr->second);
    if (streamItr->second->ssrcRewrite)
    {
        outDescription.sources.reserve(_videoSsrcs.size() + _videoPinSsrcs.size());
        for (auto& ssrcGroup : _videoSsrcs)
        {
            outDescription.sources.push_back(ssrcGroup[0]);
        }

        for (auto ssrcPair : _videoPinSsrcs)
        {
            outDescription.sources.push_back(ssrcPair);
        }
    }
    return true;
}

void Mixer::getBarbellVideoStreamDescription(std::vector<BarbellVideoStreamDescription>& outDescriptions)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    for (auto ssrcGroup : _videoSsrcs)
    {
        bridge::BarbellVideoStreamDescription description;
        description.ssrcLevels = ssrcGroup;
        description.slides = (ssrcGroup.size() == 1);
        outDescriptions.push_back(description);
    }
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
    return streamItr->second->isConfigured;
}

bool Mixer::isVideoStreamConfigured(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _videoStreams.find(endpointId);
    if (streamItr == _videoStreams.cend())
    {
        return false;
    }
    return streamItr->second->isConfigured;
}

bool Mixer::isDataStreamConfigured(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    const auto streamItr = _dataStreams.find(endpointId);
    if (streamItr == _dataStreams.cend())
    {
        return false;
    }
    return streamItr->second->isConfigured;
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

bool Mixer::getBarbellTransportDescription(const std::string& barbellId, TransportDescription& outTransportDescription)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto barbellIt = _barbells.find(barbellId);
    if (barbellIt == _barbells.end())
    {
        return false;
    }

    assert(barbellIt->second->transport);
    auto& bundleTransport = barbellIt->second->transport;

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

    auto transport = audioStreamItr->second->transport.get();
    if (!transport)
    {
        return false;
    }

    const bool isAudioConfigured = audioStreamItr->second->isConfigured;
    const bool isDtlsEnabled =
        (isAudioConfigured ? transport->isDtlsEnabled() : audioStreamItr->second->isDtlsLocalEnabled);

    if (transport->isIceEnabled() && isDtlsEnabled)
    {
        outTransportDescription = TransportDescription(transport->getLocalCandidates(),
            transport->getLocalCredentials(),
            transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && isDtlsEnabled)
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort(), transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && !isDtlsEnabled)
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

    auto transport = videoStreamItr->second->transport.get();
    if (!transport)
    {
        return false;
    }

    const bool isVideoConfigured = videoStreamItr->second->isConfigured;
    const bool isDtlsEnabled =
        (isVideoConfigured ? transport->isDtlsEnabled() : videoStreamItr->second->isDtlsLocalEnabled);

    if (transport->isIceEnabled() && isDtlsEnabled)
    {
        outTransportDescription = TransportDescription(transport->getLocalCandidates(),
            transport->getLocalCredentials(),
            transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && isDtlsEnabled)
    {
        outTransportDescription = TransportDescription(transport->getLocalRtpPort(), transport->isDtlsClient());
    }
    else if (!transport->isIceEnabled() && !isDtlsEnabled)
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
    _engine.post(utils::bind(&EngineMixer::addVideoPacketCache,
        _engineMixer.get(),
        ssrc,
        endpointIdHash,
        videoPacketCache.get()));

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
    const std::vector<uint32_t>& neighbours)
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
            static_cast<uint32_t>(rtpMap.format),
            rtpMap.payloadType);
    }
    else
    {
        logger::info("Configure audio stream, endpoint id %s, ssrc not set, rtpMap format %u, payloadType %u",
            _loggableId.c_str(),
            endpointId.c_str(),
            static_cast<uint32_t>(rtpMap.format),
            rtpMap.payloadType);
    }

    audioStream->rtpMap = rtpMap;
    audioStream->remoteSsrc = remoteSsrc;
    audioStream->transport->setAudioPayloadType(rtpMap.payloadType, rtpMap.sampleRate);

    if (audioStream->rtpMap.absSendTimeExtId.isSet())
    {
        audioStream->transport->setAbsSendTimeExtensionId(audioStream->rtpMap.absSendTimeExtId.get());
    }

    audioStream->neighbours = neighbours;
    return true;
}

bool Mixer::reconfigureAudioStream(const std::string& endpointId, const utils::Optional<uint32_t>& remoteSsrc)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto audioStreamItr = _audioStreams.find(endpointId);
    if (audioStreamItr == _audioStreams.end() || !audioStreamItr->second->isConfigured)
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
    audioStream->remoteSsrc = remoteSsrc;

    _engine.post(utils::bind(&EngineMixer::reconfigureAudioStream,
        _engineMixer.get(),
        audioStream->transport.get(),
        remoteSsrc.isSet() ? remoteSsrc.get() : 0));

    return true;
}

bool Mixer::configureVideoStream(const std::string& endpointId,
    const RtpMap& rtpMap,
    const RtpMap& feedbackRtpMap,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
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

    utils::StringBuilder<1024> ssrcsString;
    for (auto& simulcastLevel : simulcastStream.getLevels())
    {
        ssrcsString.append(simulcastLevel.ssrc);
        ssrcsString.append(",");
        ssrcsString.append(simulcastLevel.feedbackSsrc);
        ssrcsString.append(" ");
    }

    if (secondarySimulcastStream.isSet())
    {
        for (auto& simulcastLevel : secondarySimulcastStream.get().getLevels())
        {
            ssrcsString.append(simulcastLevel.ssrc);
            ssrcsString.append(",");
            ssrcsString.append(simulcastLevel.feedbackSsrc);
            ssrcsString.append(" ");
        }
    }

    utils::StringBuilder<256> ssrcWhitelistLog;
    makeSsrcWhitelistLog(ssrcWhitelist, ssrcWhitelistLog);

    logger::info("Configure video stream, endpoint id %s, ssrc %s, %s",
        _loggableId.c_str(),
        videoStream->endpointId.c_str(),
        ssrcsString.get(),
        ssrcWhitelistLog.get());

    videoStream->rtpMap = rtpMap;
    videoStream->feedbackRtpMap = feedbackRtpMap;
    videoStream->simulcastStream = simulcastStream;
    if (secondarySimulcastStream.isSet())
    {
        videoStream->secondarySimulcastStream = secondarySimulcastStream;
    }

    if (videoStream->rtpMap.absSendTimeExtId.isSet())
    {
        videoStream->transport->setAbsSendTimeExtensionId(videoStream->rtpMap.absSendTimeExtId.get());
    }

    videoStream->ssrcWhitelist = ssrcWhitelist;
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

    if (!videoStream->isConfigured)
    {
        logger::warn("Reconfigure video stream id %s, not configured", _loggableId.c_str(), endpointId.c_str());
        return false;
    }

    utils::StringBuilder<1024> ssrcsString;
    for (auto& simulcastLevel : simulcastStream.getLevels())
    {
        ssrcsString.append(simulcastLevel.ssrc);
        ssrcsString.append(",");
        ssrcsString.append(simulcastLevel.feedbackSsrc);
        ssrcsString.append(" ");
    }

    if (secondarySimulcastStream.isSet())
    {
        for (auto& simulcastLevel : secondarySimulcastStream.get().getLevels())
        {
            ssrcsString.append(simulcastLevel.ssrc);
            ssrcsString.append(",");
            ssrcsString.append(simulcastLevel.feedbackSsrc);
            ssrcsString.append(" ");
        }
    }

    utils::StringBuilder<256> ssrcWhitelistLog;
    makeSsrcWhitelistLog(ssrcWhitelist, ssrcWhitelistLog);

    logger::info("Reconfigure video stream id %s, endpoint id %s, ssrcs %s, %s",
        _loggableId.c_str(),
        videoStream->id.c_str(),
        videoStream->endpointId.c_str(),
        ssrcsString.get(),
        ssrcWhitelistLog.get());

    videoStream->simulcastStream = simulcastStream;
    videoStream->secondarySimulcastStream = secondarySimulcastStream;
    videoStream->ssrcWhitelist = ssrcWhitelist;

    _engine.post(utils::bind(&EngineMixer::reconfigureVideoStream,
        _engineMixer.get(),
        videoStream->transport.get(),
        ssrcWhitelist,
        simulcastStream,
        secondarySimulcastStream));

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

    dataStream->remoteSctpPort.set(sctpPort);
    dataStream->transport->setSctp(dataStream->localSctpPort, dataStream->remoteSctpPort.get());
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

    audioStreamItr->second->transport->setRemoteIce(credentials, candidates, _engineMixer->getAudioAllocator());
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
    videoStreamItr->second->transport->setRemoteIce(credentials, candidates, _engineMixer->getAudioAllocator());
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

    return audioStreamItr->second->transport->setRemotePeer(peer);
}

bool Mixer::configureVideoStreamTransportConnection(const std::string& endpointId, const transport::SocketAddress& peer)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    return videoStreamItr->second->transport->setRemotePeer(peer);
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
    audioStreamItr->second->transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
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
    videoStreamItr->second->transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
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
    audioStreamItr->second->transport->disableDtls();
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
    videoStreamItr->second->transport->disableDtls();
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

    transportItr->second._transport->setRemoteIce(credentials, candidates, _engineMixer->getAudioAllocator());
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

    _engine.post(utils::bind(&EngineMixer::startTransport, _engineMixer.get(), transportItr->second._transport.get()));

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

    _engine.post(utils::bind(&EngineMixer::startTransport, _engineMixer.get(), audioStream->transport.get()));

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

    _engine.post(
        utils::bind(&EngineMixer::startTransport, _engineMixer.get(), videoStreamItr->second->transport.get()));

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
        audioStream->endpointId.c_str());

    audioStream->isConfigured = true;

    auto emplaceResult = _audioEngineStreams.emplace(audioStream->endpointId,
        std::make_unique<EngineAudioStream>(audioStream->endpointId,
            audioStream->endpointIdHash,
            audioStream->localSsrc,
            audioStream->remoteSsrc,
            *(audioStream->transport.get()),
            audioStream->audioMixed,
            audioStream->rtpMap,
            audioStream->ssrcRewrite,
            audioStream->idleTimeoutSeconds,
            audioStream->neighbours));

    _engine.post(utils::bind(&EngineMixer::addAudioStream, _engineMixer.get(), emplaceResult.first->second.get()));

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
        videoStream->endpointId.c_str(),
        toString(videoStream->simulcastStream.contentType));

    videoStream->isConfigured = true;

    auto emplaceResult = _videoEngineStreams.emplace(videoStream->endpointId,
        std::make_unique<EngineVideoStream>(videoStream->endpointId,
            videoStream->endpointIdHash,
            videoStream->localSsrc,
            videoStream->simulcastStream,
            videoStream->secondarySimulcastStream,
            *(videoStream->transport.get()),
            videoStream->rtpMap,
            videoStream->feedbackRtpMap,
            videoStream->ssrcWhitelist,
            videoStream->ssrcRewrite,
            _videoPinSsrcs,
            videoStream->idleTimeoutSeconds));

    _engine.post(utils::bind(&EngineMixer::addVideoStream, _engineMixer.get(), emplaceResult.first->second.get()));

    return true;
}

bool Mixer::addDataStreamToEngine(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end() || !dataStreamItr->second->remoteSctpPort.isSet())
    {
        return false;
    }
    auto dataStream = dataStreamItr->second.get();

    logger::debug("Adding dataStream to engine, endpointId %s",
        getLoggableId().c_str(),
        dataStream->endpointId.c_str());

    dataStream->isConfigured = true;

    auto emplaceResult = _dataEngineStreams.emplace(dataStream->endpointId,
        std::make_unique<EngineDataStream>(dataStream->endpointId,
            dataStream->endpointIdHash,
            *(dataStream->transport.get()),
            dataStream->idleTimeoutSeconds));

    _engine.post(utils::bind(&EngineMixer::addDataSteam, _engineMixer.get(), emplaceResult.first->second.get()));

    return true;
}

bool Mixer::pinEndpoint(const size_t endpointIdHash, const std::string& pinnedEndpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto pinnedEndpointIdHash = utils::hash<std::string>()(pinnedEndpointId);

    _engine.post(utils::bind(&EngineMixer::pinEndpoint, _engineMixer.get(), endpointIdHash, pinnedEndpointIdHash));

    return true;
}

bool Mixer::unpinEndpoint(const size_t endpointIdHash)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    _engine.post(utils::bind(&EngineMixer::pinEndpoint, _engineMixer.get(), endpointIdHash, 0));
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
    return audioStreamItr->second->transport->isGatheringComplete();
}

bool Mixer::isVideoStreamGatheringComplete(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto videoStreamItr = _videoStreams.find(endpointId);
    if (videoStreamItr == _videoStreams.end())
    {
        return false;
    }
    return videoStreamItr->second->transport->isGatheringComplete();
}

bool Mixer::isDataStreamGatheringComplete(const std::string& endpointId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto dataStreamItr = _dataStreams.find(endpointId);
    if (dataStreamItr == _dataStreams.end())
    {
        return false;
    }
    return dataStreamItr->second->transport->isGatheringComplete();
}

void Mixer::sendEndpointMessage(const std::string& toEndpointId,
    const size_t fromEndpointIdHash,
    const std::string& message)
{
    assert(fromEndpointIdHash);
    if (message.size() >= memory::AudioPacket::maxLength())
    {
        return;
    }

    auto& audioAllocator = _engineMixer->getAudioAllocator();
    auto packet = memory::makeUniquePacket(audioAllocator, message.c_str(), message.size());
    reinterpret_cast<char*>(packet->get())[message.size()] = 0; // null terminated in packet

    std::lock_guard<std::mutex> locker(_configurationLock);

    size_t toEndpointIdHash = 0;
    if (!toEndpointId.empty())
    {
        const auto dataStreamItr = _dataStreams.find(toEndpointId);
        if (dataStreamItr == _dataStreams.end())
        {
            return;
        }
        toEndpointIdHash = dataStreamItr->second->endpointIdHash;
    }

    _engine.post(utils::bind(&EngineMixer::sendEndpointMessage,
        _engineMixer.get(),
        toEndpointIdHash,
        fromEndpointIdHash,
        utils::moveParam(packet)));
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

    bool audioStreamDeleted = _audioStreams.find(endpointId) == _audioStreams.end();
    bool videoStreamDeleted = _videoStreams.find(endpointId) == _videoStreams.end();
    bool dataStreamDeleted = _dataStreams.find(endpointId) == _dataStreams.end();

    if (bundleTransportItr != _bundleTransports.end())
    {
        if (audioStreamDeleted && videoStreamDeleted && dataStreamDeleted)
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
    if (!(recordingDescription.isAudioEnabled || recordingDescription.isVideoEnabled))
    {
        logger::error("Received a recording description without any media enabled. RecordingId %s.",
            _loggableId.c_str(),
            recordingDescription.recordingId.c_str());

        return false;
    }

    std::lock_guard<std::mutex> locker(_configurationLock);

    RecordingStream* stream = findRecordingStream(recordingDescription.recordingId);
    if (stream)
    {
        const bool wasAudioEnabled = stream->_audioActiveRecCount > 0;
        const bool wasVideoEnabled = stream->_videoActiveRecCount > 0;
        const bool wasScreenSharingEnabled = stream->_screenSharingActiveRecCount > 0;

        auto attachedRecordingDescription = stream->_attachedRecording.at(recordingDescription.recordingId);
        if (attachedRecordingDescription.isAudioEnabled != recordingDescription.isAudioEnabled)
        {
            attachedRecordingDescription.isAudioEnabled = recordingDescription.isAudioEnabled;
            recordingDescription.isAudioEnabled ? ++stream->_audioActiveRecCount : --stream->_audioActiveRecCount;
        }
        if (attachedRecordingDescription.isVideoEnabled != recordingDescription.isVideoEnabled)
        {
            attachedRecordingDescription.isVideoEnabled = recordingDescription.isVideoEnabled;
            recordingDescription.isVideoEnabled ? ++stream->_videoActiveRecCount : --stream->_videoActiveRecCount;
        }
        if (attachedRecordingDescription.isScreenSharingEnabled != recordingDescription.isScreenSharingEnabled)
        {
            attachedRecordingDescription.isScreenSharingEnabled = recordingDescription.isScreenSharingEnabled;
            recordingDescription.isScreenSharingEnabled ? ++stream->_screenSharingActiveRecCount
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
            stream->_attachedRecording.emplace(recordingDescription.recordingId, recordingDescription);

        stream->_audioActiveRecCount += recordingDescription.isAudioEnabled ? 1 : 0;
        stream->_videoActiveRecCount += recordingDescription.isVideoEnabled ? 1 : 0;
        stream->_screenSharingActiveRecCount += recordingDescription.isScreenSharingEnabled ? 1 : 0;

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

            _engine.post(
                utils::bind(&EngineMixer::addRecordingStream, _engineMixer.get(), emplaceResult.first->second.get()));
        }
        else
        {
            updateRecordingEngineStreamModalities(*stream, wasAudioEnabled, wasVideoEnabled, wasScreenSharingEnabled);
        }

        auto engineStream = _recordingEngineStreams.at(conferenceId).get();
        for (const auto& transport : stream->_transports)
        {
            _engine.post(utils::bind(&EngineMixer::addTransportToRecordingStream,
                _engineMixer.get(),
                engineStream->endpointIdHash,
                transport.second.get(),
                stream->_recEventUnackedPacketsTracker[transport.first].get()));
        }

        _engine.post(utils::bind(&EngineMixer::recordingStart,
            _engineMixer.get(),
            engineStream,
            &recordingEmplaced.first->second));

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

        _engine.post(utils::bind(&EngineMixer::updateRecordingStreamModalities,
            _engineMixer.get(),
            engineStream,
            isAudioEnabled,
            isVideoEnabled,
            isScreenSharingEnabled));
    }
}

void Mixer::addRecordingTransportsToRecordingStream(RecordingStream* recordingStream,
    const std::vector<api::RecordingChannel>& channels)
{
    for (const auto& channel : channels)
    {
        auto endpointIdHash = utils::hash<std::string>{}(channel.id);
        const auto transportEntry = recordingStream->_transports.find(endpointIdHash);
        if (transportEntry == recordingStream->_transports.end())
        {
            auto transport = _transportFactory.createForRecording(endpointIdHash,
                recordingStream->_endpointIdHash,
                transport::SocketAddress::parse(channel.host, channel.port),
                channel.aesKey,
                channel.aesSalt);

            if (transport)
            {
                auto* transportRawPointer = transport.get();
                recordingStream->_transports.emplace(endpointIdHash, std::move(transport));
                recordingStream->_recEventUnackedPacketsTracker.emplace(endpointIdHash,
                    std::make_unique<UnackedPacketsTracker>("RecEventUnackedPacketsTracker"));
                _engine.post(utils::bind(&EngineMixer::startRecordingTransport, _engineMixer.get(), transportRawPointer));
            }
            else
            {
                logger::error("Creation of recording transport has failed for channel %s:%u",
                    _loggableId.c_str(),
                    channel.host.c_str(),
                    channel.port);
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
            auto endpointIdHash = utils::hash<std::string>{}(channel.id);
            auto transportItr = stream->_transports.find(endpointIdHash);
            if (transportItr == stream->_transports.end())
            {
                continue;
            }

            _engine.post(utils::bind(&EngineMixer::removeTransportFromRecordingStream,
                _engineMixer.get(),
                engineStreamEntry->second->endpointIdHash,
                transportItr->first));
        }

        if (stream->_transports.empty())
        {
            stream->_markedForDeletion = true;
            _engine.post(utils::bind(&Engine::removeRecordingStream,
                &_engine,
                _engineMixer.get(),
                engineStreamEntry->second.get()));
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

        stream->_audioActiveRecCount -= recordingDescriptionEntry->second.isAudioEnabled ? 1 : 0;
        stream->_videoActiveRecCount -= recordingDescriptionEntry->second.isVideoEnabled ? 1 : 0;
        stream->_screenSharingActiveRecCount -= recordingDescriptionEntry->second.isScreenSharingEnabled ? 1 : 0;

        auto engineStreamEntry = _recordingEngineStreams.find(stream->_id);
        if (engineStreamEntry == _recordingEngineStreams.end())
        {
            return false;
        }

        _engine.post(utils::bind(&Engine::stopRecording,
            &_engine,
            std::ref(*_engineMixer),
            std::ref(*engineStreamEntry->second),
            std::ref(recordingDescriptionEntry->second)));

        if (stream->_attachedRecording.size() == 1)
        {
            stream->_markedForDeletion = true;
            _engine.post(utils::bind(&Engine::removeRecordingStream,
                &_engine,
                _engineMixer.get(),
                engineStreamEntry->second.get()));
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

    RecordingStream* stream = findRecordingStream(recordingDesc.recordingId);
    if (!stream)
    {
        return;
    }

    const size_t erasedCount = stream->_attachedRecording.erase(recordingDesc.recordingId);
    assert(erasedCount == 1);
}

void Mixer::engineRecordingStreamRemoved(EngineRecordingStream* engineStream)
{
    std::lock_guard<std::mutex> locker(_configurationLock);

    auto streamItr = _recordingStreams.find(engineStream->id);
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
        _engineMixer->getJobManager().abortTimedJobs(transportEntry.second->getId());

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
    _recordingEngineStreams.erase(engineStream->id);
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
    _engine.post(utils::bind(&EngineMixer::addRecordingRtpPacketCache,
        _engineMixer.get(),
        ssrc,
        endpointIdHash,
        packetCache.get()));

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
    _engineMixer->getJobManager().abortTimedJobs(transportItr->second->getId());
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

bool Mixer::addBarbell(const std::string& barbellId, ice::IceRole iceRole)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto barbellIt = _barbells.find(barbellId);
    if (barbellIt != _barbells.end())
    {
        logger::warn("Barbell with id %s already exists", _loggableId.c_str(), barbellId.c_str());
        return false;
    }

    std::shared_ptr<transport::RtcTransport> transport;
    if (_barbellPorts.empty())
    {
        _transportFactory.openRtpMuxPorts(_barbellPorts, 32);
        if (_barbellPorts.empty())
        {
            logger::error("Failed to open UDP ports for barbell", _loggableId.c_str());
            return false;
        }
    }

    transport =
        _transportFactory
            .createOnPorts(iceRole, 64, utils::hash<std::string>{}(barbellId), _barbellPorts, 128, 128, false, false);
    if (!transport)
    {
        logger::error("Failed to create transport for barbell %s", _loggableId.c_str(), barbellId.c_str());
        return false;
    }
    transport->setTag(EngineBarbell::barbellTag); // allow quick assessment that packet arrive on barbell

    const auto streamItr = _barbells.emplace(barbellId, std::make_unique<Barbell>(barbellId, transport));
    if (!streamItr.second)
    {
        logger::error("Failed to create barbell %s", _loggableId.c_str(), barbellId.c_str());
        return false;
    }

    logger::info("Created barbell id %s, hash %zu transport %s",
        _loggableId.c_str(),
        barbellId.c_str(),
        transport->getEndpointIdHash(),
        streamItr.first->second->transport->getLoggableId().c_str());

    return streamItr.first->second->transport->isInitialized();
}

bool Mixer::addBarbellToEngine(const std::string& barbellId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto barbellIt = _barbells.find(barbellId);
    if (barbellIt == _barbells.end())
    {
        return false;
    }

    logger::debug("Adding barbell to engine, id %s", getLoggableId().c_str(), barbellId.c_str());

    auto& barbell = *barbellIt->second;
    barbell.isConfigured = true;

    assert(_engineBarbells.find(barbell.id) == _engineBarbells.end());
    auto emplaceResult = _engineBarbells.emplace(barbell.id,
        std::make_unique<EngineBarbell>(barbell.id,
            *barbell.transport,
            barbell.videoSsrcs,
            barbell.audioSsrcs,
            barbell.audioRtpMap,
            barbell.videoRtpMap,
            barbell.videoFeedbackRtpMap));

    _engine.post(utils::bind(&EngineMixer::addBarbell, _engineMixer.get(), emplaceResult.first->second.get()));

    return true;
}

bool Mixer::configureBarbellTransport(const std::string& barbellId,
    const std::pair<std::string, std::string>& credentials,
    const ice::IceCandidates& candidates,
    const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool isDtlsClient)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto barbellItr = _barbells.find(barbellId);
    if (barbellItr == _barbells.end())
    {
        return false;
    }

    barbellItr->second->transport->setRemoteIce(credentials, candidates, _engineMixer->getAudioAllocator());
    barbellItr->second->transport->setRemoteDtlsFingerprint(fingerprintType, fingerprintHash, isDtlsClient);
    barbellItr->second->transport->setSctp(5000, 5000);
    return true;
}

bool Mixer::configureBarbellSsrcs(const std::string& barbellId,
    const std::vector<BarbellVideoStreamDescription>& videoSsrcs,
    const std::vector<uint32_t>& audioSsrcs,
    const bridge::RtpMap& audioRtpMap,
    const bridge::RtpMap& videoRtpMap,
    const bridge::RtpMap& videoFeedbackRtpMap)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto barbellItr = _barbells.find(barbellId);
    if (barbellItr == _barbells.end())
    {
        return false;
    }

    auto& barbell = barbellItr->second;
    barbell->audioSsrcs = audioSsrcs;
    barbell->videoSsrcs = videoSsrcs;

    barbell->audioRtpMap = audioRtpMap;
    barbell->videoRtpMap = videoRtpMap;
    barbell->videoFeedbackRtpMap = videoFeedbackRtpMap;
    return true;
}

bool Mixer::startBarbellTransport(const std::string& barbellId)
{
    std::lock_guard<std::mutex> locker(_configurationLock);
    auto barbellIt = _barbells.find(barbellId);
    if (barbellIt == _barbells.end())
    {
        return false;
    }

    _engine.post(utils::bind(&EngineMixer::startTransport, _engineMixer.get(), barbellIt->second->transport.get()));

    return true;
}

void Mixer::removeBarbell(const std::string& barbellId)
{
    auto barbellIt = _barbells.find(barbellId);
    if (barbellIt != _barbells.cend())
    {
        barbellIt->second->markedForDeletion = true;

        _engine.post(utils::bind(&EngineMixer::removeBarbell,
            _engineMixer.get(),
            barbellIt->second->transport->getEndpointIdHash()));
    }
}

void Mixer::engineBarbellRemoved(EngineBarbell* engineBarbell)
{
    std::unique_ptr<Barbell> barbell;
    {
        std::lock_guard<std::mutex> locker(_configurationLock);
        auto it = _barbells.find(engineBarbell->id);
        if (it == _barbells.cend())
        {
            return;
        }

        barbell = std::move(it->second);
        _barbells.erase(it);
        _engineBarbells.erase(barbell->id);
    }

    logTransportPacketLoss(barbell->id, *barbell->transport, _loggableId.c_str());
    if (!waitForPendingJobs(700, 5, *barbell->transport))
    {
        logger::error("Transport for barbell %s did not finish pending jobs in time. Continuing "
                      "deletion anyway.",
            _loggableId.c_str(),
            barbell->id.c_str());
    }
}

} // namespace bridge
