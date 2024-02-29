#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/ProcessMissingVideoPacketsJob.h"
#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/engine/VideoNackReceiveJob.h"
#include "rtp/RtcpFeedback.h"
#include "utils/SimpleJson.h"
#include "utils/StringBuilder.h"
#include "webrtc/DataChannel.h"

namespace bridge
{

namespace
{
template <typename TMap>
void copyToBarbellMapItemArray(utils::SimpleJsonArray& endpointArray, TMap& map)
{
    for (auto endpoint : endpointArray)
    {
        char endpointId[45];
        endpoint["endpoint-id"].getString(endpointId);
        const auto endpointIdHash = utils::hash<char*>{}(endpointId);
        auto entryIt = map.emplace(endpointIdHash, endpointId);

        auto& item = entryIt.first->second;
        for (auto ssrc : endpoint["ssrcs"].getArray())
        {
            item.newSsrcs.push_back(ssrc.getInt<uint32_t>(0));
        }

        if (endpoint.exists("noise-level"))
        {
            item.noiseLevel = endpoint["noise-level"].getFloat(0.0);
        }

        if (endpoint.exists("level"))
        {
            item.noiseLevel = endpoint["level"].getFloat(0.0);
        }

        if (endpoint.exists("neighbours"))
        {
            for (auto neighbour : endpoint["neighbours"].getArray())
            {
                item.neighbours.push_back(neighbour.getInt<uint32_t>(0));
            }
        }
    }
}

template <class T>
void addToMap(EngineBarbell::VideoStream& stream, T& videoMapping)
{
    auto* m = videoMapping.getItem(stream.endpointIdHash.get());
    if (!m)
    {
        auto entry = videoMapping.emplace(stream.endpointIdHash.get(), stream.endpointId.get().c_str());
        if (!entry.second)
        {
            return;
        }
        m = &entry.first->second;
    }

    if (m)
    {
        m->oldSsrcs.push_back(stream.stream.getKeySsrc());
    }
}

template <class T>
void addToMap(EngineBarbell::AudioStream& stream, T& audioMapping)
{
    auto* m = audioMapping.getItem(stream.endpointIdHash.get());
    if (!m)
    {
        auto entry = audioMapping.emplace(stream.endpointIdHash.get(), stream.endpointId.get().c_str());
        if (!entry.second)
        {
            return;
        }
        m = &entry.first->second;
    }

    if (m)
    {
        m->oldSsrcs.push_back(stream.ssrc);
    }
}
} // namespace

void EngineMixer::addBarbell(EngineBarbell* barbell)
{
    const auto idHash = barbell->transport.getEndpointIdHash();
    if (_engineBarbells.contains(idHash))
    {
        return;
    }

    logger::debug("Add engine barbell, transport %s, idHash %lu",
        _loggableId.c_str(),
        barbell->transport.getLoggableId().c_str(),
        idHash);

    _engineBarbells.emplace(idHash, barbell);
}

// executed on transport thread context
// remove inbound ssrc jobs should execute before this by calling decommissionInboundSsrc
void EngineMixer::internalRemoveBarbell(size_t idHash)
{
    auto barbell = _engineBarbells.getItem(idHash);
    if (!barbell)
    {
        logger::warn("barbell has already been removed from engineBarbells", _loggableId.c_str());
        return;
    }

    _engineBarbells.erase(idHash);
    _messageListener.asyncBarbellRemoved(*this, *barbell);
}

// executed on engine thread
void EngineMixer::removeBarbell(size_t idHash)
{
    auto barbell = _engineBarbells.getItem(idHash);
    if (!barbell)
    {
        return;
    }

    barbell->transport.stop();

    for (const auto& videoStream : barbell->videoStreams)
    {
        for (auto& simulcastLevel : videoStream.stream.getLevels())
        {
            decommissionInboundContext(simulcastLevel.ssrc);
            decommissionInboundContext(simulcastLevel.feedbackSsrc);
        }
        if (videoStream.endpointIdHash.isSet())
        {
            _activeMediaList->removeVideoParticipant(videoStream.endpointIdHash.get());
        }
    }

    for (auto& audioStream : barbell->audioStreams)
    {
        decommissionInboundContext(audioStream.ssrc);
        if (audioStream.endpointIdHash.isSet())
        {
            _activeMediaList->removeAudioParticipant(audioStream.endpointIdHash.get());
            _neighbourMemberships.erase(audioStream.endpointIdHash.get());
        }
    }

    barbell->transport.postOnQueue(utils::bind(&EngineMixer::internalRemoveBarbell, this, barbell->idHash));
}

void EngineMixer::checkBarbellPacketCounters(const uint64_t timestamp)
{
    for (auto& barbellIt : _engineBarbells)
    {
        auto barbell = barbellIt.second;
        const auto packetCount = barbell->transport.getInboundPacketCount();
        if (barbell->inboundPackets.timestamp == 0)
        {
            barbell->inboundPackets.count = packetCount;
            barbell->inboundPackets.timestamp = timestamp;
        }
        else if (utils::Time::diffGE(barbell->inboundPackets.timestamp, timestamp, utils::Time::sec * 15))
        {
            if (barbell->inboundPackets.count == packetCount)
            {
                removeBarbell(barbell->idHash);
            }
            else
            {
                barbell->inboundPackets.count = packetCount;
                barbell->inboundPackets.timestamp = timestamp;
            }
        }
    }
}

// This method must be executed on engine thread. UMM requests could go in another queue and processed
// at start of tick.
// There are three phases to update the endpoints and ssrc mappings in active media list
// Step1: Create a table of all existing stream mappings and all new stream mappings so we can compare changes
// Step2: Remove all streams that have changed mapping and old mapping. Could be that the new mapping is nil
// Step3: Add all streams that have changed and new mapping
void EngineMixer::onBarbellUserMediaMap(size_t barbellIdHash, const char* message)
{
    auto barbell = _engineBarbells.getItem(barbellIdHash);
    if (!barbell)
    {
        logger::debug("cannot find barbell for UMM. %zu", _loggableId.c_str(), barbellIdHash);
        return;
    }

    auto mediaMapJson = utils::SimpleJson::create(message, strlen(message));

    logger::info("received BB msg over barbell %s %zu, json %s",
        _loggableId.c_str(),
        barbell->id.c_str(),
        barbellIdHash,
        message);

    auto videoEndpointsArray = mediaMapJson["video-endpoints"].getArray();
    auto audioEndpointsArray = mediaMapJson["audio-endpoints"].getArray();

    memory::Map<size_t, BarbellMapItem, 16> videoSsrcs;
    copyToBarbellMapItemArray(videoEndpointsArray, videoSsrcs);
    for (auto& stream : barbell->videoStreams)
    {
        if (stream.endpointIdHash.isSet())
        {
            addToMap(stream, videoSsrcs);
        }
    }

    memory::Map<size_t, BarbellMapItem, 8> audioSsrcs;
    copyToBarbellMapItemArray(audioEndpointsArray, audioSsrcs);
    for (auto& stream : barbell->audioStreams)
    {
        if (stream.endpointIdHash.isSet())
        {
            addToMap(stream, audioSsrcs);
        }
    }

    const auto mapRevision = _activeMediaList->getMapRevision();

    // remove video
    for (const auto& entry : videoSsrcs)
    {
        const auto& item = entry.second;
        if (item.hasChanged() && !item.oldSsrcs.empty())
        {
            _activeMediaList->removeVideoParticipant(entry.first);
            _engineStreamDirector->removeParticipant(entry.first);

            for (const auto ssrc : item.oldSsrcs)
            {
                auto* videoStream = barbell->videoSsrcMap.getItem(ssrc);
                if (videoStream)
                {
                    videoStream->endpointIdHash.clear();
                    videoStream->endpointId.clear();
                    videoStream->stream.highestActiveLevel = 0;
                }
            }
        }
    }

    // add videos
    for (const auto& entry : videoSsrcs)
    {
        const auto& item = entry.second;
        if (item.hasChanged() && !item.newSsrcs.empty())
        {
            SimulcastStream primary;
            utils::Optional<SimulcastStream> secondary;
            uint32_t streamIndex = 0;
            for (const auto ssrc : item.newSsrcs)
            {
                auto* videoStream = barbell->videoSsrcMap.getItem(ssrc);
                if (!videoStream)
                {
                    assert(false);
                    logger::error("unannounced video ssrc %u", _loggableId.c_str(), ssrc);
                    continue;
                }

                videoStream->stream.highestActiveLevel = 0;

                if (streamIndex++ == 0)
                {
                    primary = videoStream->stream;
                }
                else
                {
                    secondary.set(videoStream->stream);
                }
                videoStream->endpointIdHash.set(entry.first);
                videoStream->endpointId.set(item.endpointId);
            }

            _activeMediaList->addBarbellVideoParticipant(entry.first, primary, secondary, item.endpointId);
            _engineStreamDirector->addParticipant(entry.first, primary, secondary.isSet() ? &secondary.get() : nullptr);

            for (const auto& simLevel : primary.getLevels())
            {
                auto* inboundContext = _ssrcInboundContexts.getItem(simLevel.ssrc);
                if (inboundContext)
                {
                    inboundContext->makeReady();
                    inboundContext->pliScheduler.triggerPli();
                }
            }
            if (secondary.isSet())
            {
                for (const auto& simLevel : secondary.get().getLevels())
                {
                    auto* inboundContext = _ssrcInboundContexts.getItem(simLevel.ssrc);
                    if (inboundContext)
                    {
                        inboundContext->makeReady();
                        inboundContext->pliScheduler.triggerPli();
                    }
                }
            }
        }
    }

    const auto audioMapRevision = _activeMediaList->getMapRevision();

    // remove audio
    for (const auto& entry : audioSsrcs)
    {
        const auto& item = entry.second;
        if (item.hasChanged() && !item.oldSsrcs.empty())
        {
            _activeMediaList->removeAudioParticipant(entry.first);
            _neighbourMemberships.erase(entry.first);
            for (const auto ssrc : item.oldSsrcs)
            {
                auto* audioStream = barbell->audioSsrcMap.getItem(ssrc);
                if (audioStream)
                {
                    audioStream->endpointIdHash.clear();
                    audioStream->endpointId.clear();
                }
            }
        }
    }

    // add audio
    for (const auto& entry : audioSsrcs)
    {
        const auto& item = entry.second;
        if (item.hasChanged() && !item.newSsrcs.empty())
        {
            auto* audioStream = barbell->audioSsrcMap.getItem(item.newSsrcs[0]);
            if (!audioStream)
            {
                logger::error("unannounced audio ssrc %u", _loggableId.c_str(), item.newSsrcs[0]);
                // if it is set it must already be set to this endpointId
                continue;
            }

            audioStream->endpointIdHash.set(entry.first);
            audioStream->endpointId.set(item.endpointId);
            _activeMediaList->addBarbellAudioParticipant(entry.first,
                item.endpointId,
                item.noiseLevel,
                item.recentLevel);
            if (!item.neighbours.empty())
            {
                auto neighbourIt = _neighbourMemberships.emplace(entry.first, entry.first);
                if (neighbourIt.second)
                {
                    auto& newMembershipItem = neighbourIt.first->second;
                    utils::append(newMembershipItem.memberships, item.neighbours);
                }
            }
        }
    }

    if (logger::_logLevel >= logger::Level::DBG && audioMapRevision != _activeMediaList->getMapRevision())
    {
        _activeMediaList->logAudioList();
    }

    if (mapRevision != _activeMediaList->getMapRevision())
    {
        // only update local users
        sendUserMediaMapMessageToAll();
    }
}

void EngineMixer::onBarbellMinUplinkEstimate(size_t barbellIdHash, const char* message)
{
    auto barbell = _engineBarbells.getItem(barbellIdHash);
    if (!barbell)
    {
        return;
    }

    logger::debug("received BB msg over barbell %s %zu, json %s",
        _loggableId.c_str(),
        barbell->id.c_str(),
        barbellIdHash,
        message);

    barbell->minClientDownlinkBandwidth =
        api::DataChannelMessageParser::getMinUplinkBitrate(utils::SimpleJson::create(message, strlen(message)));
}

void EngineMixer::onBarbellDataChannelEstablish(size_t barbellIdHash,
    webrtc::SctpStreamMessageHeader& header,
    size_t packetSize)
{
    auto barbell = _engineBarbells.getItem(barbellIdHash);
    if (!barbell)
    {
        return;
    }

    const auto state = barbell->dataChannel.getState();
    barbell->dataChannel.onSctpMessage(&barbell->transport,
        header.id,
        header.sequenceNumber,
        header.payloadProtocol,
        header.data(),
        header.getMessageLength(packetSize));

    const auto newState = barbell->dataChannel.getState();
    if (state != newState && newState == webrtc::WebRtcDataStream::State::OPEN)
    {
        sendUserMediaMapMessageOverBarbells();
    }
}

SsrcInboundContext* EngineMixer::emplaceBarbellInboundSsrcContext(const uint32_t ssrc,
    transport::RtcTransport* sender,
    const uint32_t payloadType,
    const uint64_t timestamp)
{
    const auto endpointIdHash = sender->getEndpointIdHash();
    auto* barbell = _engineBarbells.getItem(endpointIdHash);
    if (!barbell)
    {
        return nullptr;
    }

    auto videoStream = barbell->videoSsrcMap.getItem(ssrc);
    if (videoStream)
    {
        const RtpMap& videoRtpMap =
            barbell->videoRtpMap.payloadType == payloadType ? barbell->videoRtpMap : barbell->videoFeedbackRtpMap;
        auto simulcastLevel = videoStream->stream.getLevelOf(ssrc);
        if (barbell->videoRtpMap.payloadType == payloadType && !simulcastLevel.isSet())
        {
            logger::error("ssrc %u is not in simulcast group of barbell video stream %zu",
                _loggableId.c_str(),
                ssrc,
                barbell->idHash);
            return nullptr;
        }

        auto emplaceResult = _allSsrcInboundContexts.emplace(ssrc,
            ssrc,
            videoRtpMap,
            sender,
            timestamp,
            simulcastLevel.get(),
            videoStream->stream.levels[0].ssrc);
        if (!emplaceResult.second && emplaceResult.first == _allSsrcInboundContexts.end())
        {
            logger::error("Failed to create barbell inbound video context for ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }

        _ssrcInboundContexts.emplace(ssrc, &emplaceResult.first->second);

        logger::info("Created new barbell inbound video context for stream ssrc %u, endpointIdHash %zu, %s",
            _loggableId.c_str(),
            ssrc,
            endpointIdHash,
            sender->getLoggableId().c_str());
        return &emplaceResult.first->second;
    }

    if (barbell->audioSsrcMap.contains(ssrc))
    {
        auto emplaceResult =
            _allSsrcInboundContexts.emplace(ssrc, ssrc, barbell->audioRtpMap, bridge::RtpMap::EMPTY, sender, timestamp);

        if (!emplaceResult.second && emplaceResult.first == _allSsrcInboundContexts.end())
        {
            logger::error("Failed to create barbell inbound audio context for ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }
        _ssrcInboundContexts.emplace(ssrc, &emplaceResult.first->second);

        logger::info("Created new barbell inbound audio context for stream ssrc %u, endpointIdHash %zu, %s",
            _loggableId.c_str(),
            ssrc,
            endpointIdHash,
            sender->getLoggableId().c_str());
        return &emplaceResult.first->second;
    }

    return nullptr;
}

void EngineMixer::processBarbellSctp(const uint64_t timestamp)
{
    for (IncomingPacketInfo packetInfo; _incomingBarbellSctp.pop(packetInfo);)
    {
        auto header = reinterpret_cast<webrtc::SctpStreamMessageHeader*>(packetInfo.packet()->get());

        if (header->payloadProtocol == webrtc::DataChannelPpid::WEBRTC_STRING)
        {
            auto message = reinterpret_cast<const char*>(header->data());
            const auto messageLength = header->getMessageLength(packetInfo.packet()->getLength());
            if (messageLength == 0)
            {
                return;
            }

            auto messageJson = utils::SimpleJson::create(message, messageLength);

            if (api::DataChannelMessageParser::isUserMediaMap(messageJson))
            {
                onBarbellUserMediaMap(packetInfo.transport()->getEndpointIdHash(), message);
            }
            else if (api::DataChannelMessageParser::isMinUplinkBitrate(messageJson))
            {
                onBarbellMinUplinkEstimate(packetInfo.transport()->getEndpointIdHash(), message);
            }
        }
        else if (header->payloadProtocol == webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
        {
            onBarbellDataChannelEstablish(packetInfo.transport()->getEndpointIdHash(),
                *header,
                packetInfo.packet()->getLength());
        }
    }
}

utils::Optional<uint32_t> EngineMixer::findBarbellMainSsrc(size_t barbellIdHash, uint32_t feedbackSsrc)
{
    auto barbell = _engineBarbells.getItem(barbellIdHash);
    if (!barbell)
    {
        return utils::Optional<uint32_t>();
    }

    auto* videoStream = barbell->videoSsrcMap.getItem(feedbackSsrc);
    if (!videoStream)
    {
        assert(false);
        return utils::Optional<uint32_t>();
    }
    return videoStream->stream.getMainSsrcFor(feedbackSsrc);
}

void EngineMixer::processBarbellMissingPackets(bridge::SsrcInboundContext& ssrcInboundContext)
{
    const auto barbell = _engineBarbells.getItem(ssrcInboundContext.sender->getEndpointIdHash());
    if (barbell)
    {
        const uint32_t REPORTER_SSRC = 0;
        auto videoStream = barbell->videoSsrcMap.getItem(ssrcInboundContext.ssrc);
        if (videoStream)
        {
            barbell->transport.getJobQueue().addJob<bridge::ProcessMissingVideoPacketsJob>(ssrcInboundContext,
                REPORTER_SSRC,
                barbell->transport,
                _sendAllocator);
        }
    }
}

void EngineMixer::processIncomingBarbellFbRtcpPacket(EngineBarbell& barbell,
    const rtp::RtcpFeedback& rtcpFeedback,
    const uint64_t timestamp)
{
    uint32_t rtxSsrc = 0;
    const auto mediaSsrc = rtcpFeedback.mediaSsrc.get();
    _activeMediaList->getFeedbackSsrc(mediaSsrc, rtxSsrc);

    auto& bbTransport = barbell.transport;
    auto* mediaSsrcOutboundContext = obtainOutboundSsrcContext(barbell.idHash,
        barbell.ssrcOutboundContexts,
        mediaSsrc,
        barbell.videoRtpMap,
        bridge::RtpMap::EMPTY);
    if (!mediaSsrcOutboundContext)
    {
        return;
    }

    auto* rtxSsrcOutboundContext = obtainOutboundSsrcContext(barbell.idHash,
        barbell.ssrcOutboundContexts,
        rtxSsrc,
        barbell.videoFeedbackRtpMap,
        bridge::RtpMap::EMPTY);

    if (!rtxSsrcOutboundContext)
    {
        return;
    }

    mediaSsrcOutboundContext->onRtpSent(timestamp);
    const auto numFeedbackControlInfos = rtp::getNumFeedbackControlInfos(&rtcpFeedback);
    uint16_t pid = 0;
    uint16_t blp = 0;

    auto nackPacketCount = getNackPacketCount(&rtcpFeedback, numFeedbackControlInfos);

    logger::debug("Barbell received NACK for %zu pkts on ssrc %u, %s",
        _loggableId.c_str(),
        nackPacketCount,
        mediaSsrc,
        barbell.transport.getLoggableId().c_str());

    for (size_t i = 0; i < numFeedbackControlInfos; ++i)
    {
        rtxSsrcOutboundContext->onRtpSent(timestamp);
        rtp::getFeedbackControlInfo(&rtcpFeedback, i, numFeedbackControlInfos, pid, blp);
        bbTransport.getJobQueue().addJob<bridge::VideoNackReceiveJob>(*rtxSsrcOutboundContext,
            bbTransport,
            *mediaSsrcOutboundContext,
            pid,
            blp,
            timestamp,
            barbell.transport.getRtt());
    }
}

void EngineMixer::sendUserMediaMapMessageOverBarbells()
{
    _lastSendTimeOfUserMediaMapMessageOverBarbells = _lastStartedIterationTimestamp;

    if (_engineBarbells.size() == 0)
    {
        return;
    }

    utils::StringBuilder<1024> userMediaMapMessage;
    _activeMediaList->makeBarbellUserMediaMapMessage(userMediaMapMessage, _neighbourMemberships);

    logger::debug("send BB msg %s", _loggableId.c_str(), userMediaMapMessage.get());

    for (auto& barbell : _engineBarbells)
    {
        if (barbell.second->dataChannel.isOpen())
        {
            barbell.second->dataChannel.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
        }
    }
}

void EngineMixer::sendPeriodicUserMediaMapMessageOverBarbells(const uint64_t engineIterationStartTimestamp)
{
    // Send periodic messages over barbell. It can be useful for recovery scenarios where for a period of time
    // there are more than 1 barbell connection between the same SMB and decommission of the broken barbell streams
    // can remove the participants from active list.
    // This is disabled by default and it's possible to be enabled by
    if (!_engineBarbells.empty() && _config.barbell.userMapPeriodicSendingInterval > 0)
    {
        const int64_t interval =
            static_cast<uint64_t>(_config.barbell.userMapPeriodicSendingInterval) * utils::Time::sec;
        if (utils::Time::diffGE(_lastSendTimeOfUserMediaMapMessageOverBarbells,
                engineIterationStartTimestamp,
                interval))
        {
            sendUserMediaMapMessageOverBarbells();
        }
    }
}

bool EngineMixer::setPacketSourceEndpointIdHash(memory::Packet& packet,
    size_t barbellIdHash,
    uint32_t ssrc,
    bool isAudio)
{
    auto* barbell = _engineBarbells.getItem(barbellIdHash);
    if (!barbell)
    {
        return false;
    }

    if (isAudio)
    {
        const auto* audioStream = barbell->audioSsrcMap.getItem(ssrc);
        if (audioStream && audioStream->endpointIdHash.isSet())
        {
            packet.endpointIdHash = audioStream->endpointIdHash.get();
            return true;
        }
    }
    else
    {
        const auto* videoStream = barbell->videoSsrcMap.getItem(ssrc);
        if (videoStream && videoStream->endpointIdHash.isSet())
        {
            packet.endpointIdHash = videoStream->endpointIdHash.get();
            return true;
        }
    }

    return false;
}

void EngineMixer::forwardAudioRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()))
    {
        return;
    }

    for (auto& it : _engineBarbells)
    {
        auto& barbell = *it.second;
        const auto& audioSsrcRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
        const auto rewriteMapItr = audioSsrcRewriteMap.find(packetInfo.packet()->endpointIdHash);
        if (rewriteMapItr == audioSsrcRewriteMap.end())
        {
            continue;
        }
        uint32_t ssrc = rewriteMapItr->second;

        auto* ssrcOutboundContext = obtainOutboundSsrcContext(barbell.idHash,
            barbell.ssrcOutboundContexts,
            ssrc,
            barbell.audioRtpMap,
            bridge::RtpMap::EMPTY);

        if (!ssrcOutboundContext)
        {
            continue;
        }

        auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
        if (packet)
        {
            barbell.transport.getJobQueue().addJob<AudioForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                *(packetInfo.inboundContext()),
                std::move(packet),
                packetInfo.extendedSequenceNumber(),
                barbell.transport,
                timestamp);
        }
        else
        {
            logger::warn("send allocator depleted. forwardAudioRtpPacketOverBarbell", _loggableId.c_str());
        }
    }
}

void EngineMixer::forwardVideoRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()) || !packetInfo.inboundContext())
    {
        return;
    }

    const auto senderEndpointIdHash = packetInfo.packet()->endpointIdHash;
    for (auto& it : _engineBarbells)
    {
        auto& barbell = *it.second;

        uint32_t targetSsrc = 0;
        const auto simulcastLevel = packetInfo.inboundContext()->simulcastLevel;
        const auto& screenShareSsrcMapping = _activeMediaList->getVideoScreenShareSsrcMapping();

        if (screenShareSsrcMapping.isSet() && screenShareSsrcMapping.get().first == senderEndpointIdHash &&
            screenShareSsrcMapping.get().second.ssrc == packetInfo.inboundContext()->ssrc)
        {
            targetSsrc = screenShareSsrcMapping.get().second.rewriteSsrc;
        }
        else
        {
            const auto& videoSsrcRewriteMap = _activeMediaList->getVideoSsrcRewriteMap();
            const auto* rewriteMapping = videoSsrcRewriteMap.getItem(senderEndpointIdHash);
            if (!rewriteMapping)
            {
                continue;
            }

            targetSsrc = (*rewriteMapping)[simulcastLevel].main;
        }

        auto* ssrcOutboundContext =
            obtainOutboundSsrcContext(barbell.idHash, barbell.ssrcOutboundContexts, targetSsrc, barbell.videoRtpMap, bridge::RtpMap::EMPTY);

        if (!ssrcOutboundContext)
        {
            continue;
        }

        ssrcOutboundContext->onRtpSent(timestamp); // marks that we have active jobs on this ssrc context
        auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
        if (packet)
        {
            barbell.transport.getJobQueue().addJob<VideoForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                *(packetInfo.inboundContext()),
                std::move(packet),
                barbell.transport,
                packetInfo.extendedSequenceNumber(),
                _messageListener,
                barbell.idHash,
                *this,
                timestamp);
        }
        else
        {
            logger::warn("send allocator depleted fwdVideoOverBarbell", _loggableId.c_str());
        }
    }
}

void EngineMixer::reportMinRemoteClientDownlinkBandwidthToBarbells(const uint32_t minUplinkEstimate) const
{
    utils::StringBuilder<1024> message;
    api::DataChannelMessage::makeMinUplinkBitrate(message, minUplinkEstimate);

    if (!_engineBarbells.empty())
    {
        logger::debug("send BB msg %s", _loggableId.c_str(), message.get());
    }
    for (const auto& itBb : _engineBarbells)
    {
        itBb.second->dataChannel.sendString(message.get(), message.getLength());
    }
}

bool EngineMixer::asyncAddBarbell(EngineBarbell* barbell)
{
    return post(utils::bind(&EngineMixer::addBarbell, this, barbell));
}

bool EngineMixer::asyncRemoveBarbell(size_t idHash)
{
    return post(utils::bind(&EngineMixer::removeBarbell, this, idHash));
}
} // namespace bridge
