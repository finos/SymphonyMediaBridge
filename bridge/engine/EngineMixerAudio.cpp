#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/FinalizeNonSsrcRewriteOutboundContextJob.h"
#include "bridge/engine/SendRtcpJob.h"
#include "bridge/engine/TelephoneEventForwardReceiveJob.h"
#include "codec/AudioTools.h"
#include "codec/Opus.h"
#include "config/Config.h"

namespace
{
const double mixSampleScaleFactor = 0.5;

template <class V, class T>
bool isNeighbour(const V& groupList, const T& lookupTable)
{
    for (auto& entry : groupList)
    {
        if (lookupTable.contains(entry))
        {
            return true;
        }
    }
    return false;
}

template <class TMap>
bool areNeighbours(const TMap& table1, const TMap& table2)
{
    if (table1.size() < table2.size())
    {
        for (auto& entry : table1)
        {
            if (table2.contains(entry.first))
            {
                return true;
            }
        }
    }
    else
    {
        for (auto& entry : table2)
        {
            if (table1.contains(entry.first))
            {
                return true;
            }
        }
    }

    return false;
}

bool isContributingToMix(const bridge::SsrcInboundContext* inboundAudioContext)
{
    return inboundAudioContext && inboundAudioContext->hasAudioReceivePipe.load() &&
        inboundAudioContext->audioReceivePipe->getAudioSampleCount() > 0;
}

} // namespace

namespace bridge
{

void EngineMixer::addAudioStream(EngineAudioStream* engineAudioStream)
{
    const auto endpointIdHash = engineAudioStream->transport.getEndpointIdHash();
    if (_engineAudioStreams.contains(endpointIdHash))
    {
        return;
    }

    logger::debug("Add engineAudioStream, transport %s, endpointIdHash %lu, audioMixed %c",
        _loggableId.c_str(),
        engineAudioStream->transport.getLoggableId().c_str(),
        endpointIdHash,
        engineAudioStream->isMixed() ? 't' : 'f');

    _engineAudioStreams.emplace(endpointIdHash, engineAudioStream);
    if (engineAudioStream->isMixed())
    {
        _numMixedAudioStreams++;
    }

    auto neighbourIt = _neighbourMemberships.emplace(endpointIdHash, endpointIdHash);
    if (neighbourIt.second)
    {
        auto& neighbourList = neighbourIt.first->second.memberships;
        for (auto& it : engineAudioStream->neighbours)
        {
            neighbourList.push_back(it.first);
        }
    }
    else
    {
        logger::error("Failed to setup neighbour list for audio stream %zu", _loggableId.c_str(), endpointIdHash);
    }

    const auto mapRevision = _activeMediaList->getMapRevision();
    _activeMediaList->addAudioParticipant(endpointIdHash, engineAudioStream->endpointId.c_str());
    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageToAll();
        sendUserMediaMapMessageOverBarbells();
    }
    updateBandwidthFloor();

    sendAudioStreamToRecording(*engineAudioStream, true);
}

void EngineMixer::removeStream(const EngineAudioStream* engineAudioStream)
{
    const auto endpointIdHash = engineAudioStream->endpointIdHash;

    const auto mapRevision = _activeMediaList->getMapRevision();
    _activeMediaList->removeAudioParticipant(endpointIdHash);
    _neighbourMemberships.erase(endpointIdHash);

    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageToAll();
        sendUserMediaMapMessageOverBarbells();
    }

    updateBandwidthFloor();

    if (engineAudioStream->transport.isConnected())
    {
        // Job count will increase delaying the deletion of transport by Mixer.
        auto* context = engineAudioStream->ssrcOutboundContexts.getItem(engineAudioStream->localSsrc);
        if (context)
        {
            auto goodByePacket = createGoodBye(context->ssrc, context->allocator);
            if (goodByePacket)
            {
                engineAudioStream->transport.getJobQueue().addJob<SendRtcpJob>(std::move(goodByePacket),
                    engineAudioStream->transport);
            }
        }
    }

    if (engineAudioStream->isMixed())
    {
        _numMixedAudioStreams--;
        if (_numMixedAudioStreams == 0)
        {
            for (auto& audioStreamIt : _engineAudioStreams)
            {
                if (audioStreamIt.second->remoteSsrc.isSet())
                {
                    auto ssrcContext = _ssrcInboundContexts.getItem(audioStreamIt.second->remoteSsrc.get());
                    if (ssrcContext && ssrcContext->hasAudioReceivePipe.load())
                    {
                        audioStreamIt.second->transport.postOnQueue(
                            [ssrcContext]() { ssrcContext->audioReceivePipe->flush(); });
                    }
                }
            }
        }
    }

    logger::debug("Remove engineAudioStream, transport %s",
        _loggableId.c_str(),
        engineAudioStream->transport.getLoggableId().c_str());

    if (engineAudioStream->remoteSsrc.isSet())
    {
        decommissionInboundContext(engineAudioStream->remoteSsrc.get());

        markAssociatedAudioOutboundContextsForDeletion(engineAudioStream);
        sendAudioStreamToRecording(*engineAudioStream, false);
    }

    _engineAudioStreams.erase(endpointIdHash);

    engineAudioStream->transport.postOnQueue(
        [this, engineAudioStream]() { _messageListener.asyncAudioStreamRemoved(*this, *engineAudioStream); });
}

void EngineMixer::reconfigureAudioStream(const transport::RtcTransport& transport, const uint32_t remoteSsrc)
{
    auto* engineAudioStream = _engineAudioStreams.getItem(transport.getEndpointIdHash());
    if (!engineAudioStream)
    {
        return;
    }

    if (engineAudioStream->remoteSsrc.isSet() && engineAudioStream->remoteSsrc.get() != remoteSsrc)
    {
        decommissionInboundContext(engineAudioStream->remoteSsrc.get());
        sendAudioStreamToRecording(*engineAudioStream, false);
    }

    if (remoteSsrc != 0)
    {
        engineAudioStream->remoteSsrc.set(remoteSsrc);
        sendAudioStreamToRecording(*engineAudioStream, true);
    }
    else
    {
        engineAudioStream->remoteSsrc = utils::Optional<uint32_t>();
    }
    updateBandwidthFloor();
}

void EngineMixer::reconfigureNeighbours(const transport::RtcTransport& transport,
    const std::vector<uint32_t>& neighbourList)
{
    auto* engineAudioStream = _engineAudioStreams.getItem(transport.getEndpointIdHash());
    if (!engineAudioStream)
    {
        return;
    }

    engineAudioStream->neighbours.clear();

    for (auto& neighbour : neighbourList)
    {
        engineAudioStream->neighbours.add(neighbour, true);
    }

    const auto endpointIdHash = engineAudioStream->endpointIdHash;
    auto neighbourIt = _neighbourMemberships.find(endpointIdHash);
    if (neighbourIt != _neighbourMemberships.end())
    {
        auto& neighbourList = neighbourIt->second.memberships;
        neighbourList.clear();
        for (auto& it : engineAudioStream->neighbours)
        {
            neighbourList.push_back(it.first);
        }

        sendUserMediaMapMessageOverBarbells();
    }
    else
    {
        logger::error("Failed to update neighbour list for audio stream %zu", _loggableId.c_str(), endpointIdHash);
    }
}

void EngineMixer::onAudioRtpPacketReceived(SsrcInboundContext& ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    if (!_engineAudioStreams.empty())
    {
        sender->getJobQueue().addJob<bridge::AudioForwarderReceiveJob>(std::move(packet),
            sender,
            *this,
            ssrcContext,
            *_activeMediaList,
            _config.audio.silenceThresholdLevel,
            _numMixedAudioStreams != 0,
            !_engineBarbells.empty() || _engineAudioStreams.size() > 2,
            extendedSequenceNumber);
    }
}

void EngineMixer::onAudioTelephoneEventRtpPacketReceived(SsrcInboundContext& ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    if (!_engineAudioStreams.empty())
    {
        sender->getJobQueue().addJob<bridge::TelephoneEventForwardReceiveJob>(std::move(packet),
            sender,
            *this,
            ssrcContext,
            extendedSequenceNumber);
    }
}

void EngineMixer::onForwarderAudioRtpPacketDecrypted(SsrcInboundContext& inboundContext,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber)
{
    assert(packet);
    if (!_incomingForwarderAudioRtp.push(
            IncomingPacketInfo(std::move(packet), &inboundContext, extendedSequenceNumber)))
    {
        logger::error("Failed to push incoming forwarder audio packet onto queue", getLoggableId().c_str());
        assert(false);
    }
}

void EngineMixer::forwardAudioRtpPacket(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
    auto srcUserId = getC9UserId(rtpHeader->ssrc);

    const auto& audioSsrcRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    const auto rewriteMapItr = audioSsrcRewriteMap.find(packetInfo.packet()->endpointIdHash);
    const bool sourceMapped = (rewriteMapItr != audioSsrcRewriteMap.end());
    const auto originalSsrc = packetInfo.inboundContext()->ssrc;

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto* audioStream = audioStreamEntry.second;

        if (audioStream && &audioStream->transport == packetInfo.transport())
        {
            if (!audioStream->detectedAudioSsrc.isSet())
            {
                logger::info("%zu detected audio ssrc %u, negotiated ssrc %u, audio-lvl-extid %u",
                    _loggableId.c_str(),
                    audioStream->endpointIdHash,
                    originalSsrc,
                    audioStream->remoteSsrc.get(),
                    audioStream->rtpMap.audioLevelExtId.valueOr(0));
            }
            audioStream->detectedAudioSsrc.set(originalSsrc);
        }
        if (!audioStream || &audioStream->transport == packetInfo.transport() || audioStream->isMixed())
        {
            continue;
        }
        if (audioStream->mediaMode == MediaMode::SSRC_REWRITE && !sourceMapped)
        {
            continue;
        }

        if (srcUserId.isSet() && audioStream->neighbours.contains(srcUserId.get()))
        {
            continue;
        }

        if (!audioStream->neighbours.empty())
        {
            auto* srcMemberships = _neighbourMemberships.getItem(packetInfo.packet()->endpointIdHash);
            if (srcMemberships && isNeighbour(srcMemberships->memberships, audioStream->neighbours))
            {
                continue;
            }
        }

        SsrcOutboundContext* ssrcOutboundContext = nullptr;
        if (audioStream->mediaMode == MediaMode::SSRC_REWRITE)
        {
            ssrcOutboundContext = obtainOutboundSsrcContext(audioStream->endpointIdHash,
                audioStream->ssrcOutboundContexts,
                rewriteMapItr->second,
                audioStream->rtpMap,
                audioStream->telephoneEventRtpMap);
        }
        else
        {
            ssrcOutboundContext = obtainOutboundForwardSsrcContext(audioStream->endpointIdHash,
                audioStream->ssrcOutboundContexts,
                originalSsrc,
                audioStream->rtpMap,
                audioStream->telephoneEventRtpMap);
        }

        if (!ssrcOutboundContext)
        {
            continue;
        }

        auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
        if (packet)
        {
            audioStream->transport.getJobQueue().addJob<AudioForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                *(packetInfo.inboundContext()),
                std::move(packet),
                packetInfo.extendedSequenceNumber(),
                audioStream->transport,
                timestamp);
        }
        else
        {
            logger::warn("send allocator depleted. forwardAudioRtpPacket", _loggableId.c_str());
        }
    }
}

void EngineMixer::processAudioStreams()
{
    if (_numMixedAudioStreams == 0)
    {
        return;
    }

    const auto payloadBytesPerPacket =
        samplesPerFrame20ms * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;

    std::memset(_mixedData, 0, payloadBytesPerPacket);

    for (auto& ssrcContext : _ssrcInboundContexts)
    {
        if (ssrcContext.second->rtpMap.isAudio() && ssrcContext.second->hasAudioReceivePipe.load())
        {
            auto& rcvPipe = *ssrcContext.second->audioReceivePipe;
            const auto readySamples = rcvPipe.fetchStereo(samplesPerFrame20ms);
            if (readySamples > 0)
            {
                codec::addToMix(rcvPipe.getAudio(),
                    _mixedData,
                    readySamples * codec::Opus::channelsPerFrame,
                    mixSampleScaleFactor);
            }
        }
    }

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto audioStream = audioStreamEntry.second;

        if (!audioStream->isMixed())
        {
            continue;
        }

        auto audioPacket = memory::makeUniquePacket(_audioAllocator);
        if (!audioPacket)
        {
            return;
        }

        auto rtpHeader = rtp::RtpHeader::create(*audioPacket);
        rtpHeader->ssrc = audioStream->localSsrc;

        auto payloadStart = reinterpret_cast<int16_t*>(rtpHeader->getPayload());
        const auto headerLength = rtpHeader->headerLength();
        audioPacket->setLength(headerLength + payloadBytesPerPacket);
        std::memcpy(payloadStart, _mixedData, payloadBytesPerPacket);

        SsrcInboundContext* inboundAudioContext =
            (audioStream->remoteSsrc.isSet() ? _ssrcInboundContexts.getItem(audioStream->remoteSsrc.get()) : nullptr);

        if (!audioStream->neighbours.empty())
        {
            for (auto& stream : _engineAudioStreams)
            {
                auto& peerAudioStream = *stream.second;
                if (peerAudioStream.remoteSsrc.isSet() &&
                    areNeighbours(audioStream->neighbours, peerAudioStream.neighbours))
                {
                    auto neighbourContext = _ssrcInboundContexts.getItem(peerAudioStream.remoteSsrc.get());
                    if (isContributingToMix(neighbourContext))
                    {
                        codec::subtractFromMix(neighbourContext->audioReceivePipe->getAudio(),
                            payloadStart,
                            neighbourContext->audioReceivePipe->getAudioSampleCount() * codec::Opus::channelsPerFrame,
                            mixSampleScaleFactor);
                    }
                }
            }
        }
        else if (inboundAudioContext && isContributingToMix(inboundAudioContext))
        {
            codec::subtractFromMix(inboundAudioContext->audioReceivePipe->getAudio(),
                payloadStart,
                inboundAudioContext->audioReceivePipe->getAudioSampleCount() * codec::Opus::channelsPerFrame,
                mixSampleScaleFactor);
        }

        auto* ssrcContext = obtainOutboundSsrcContext(audioStream->endpointIdHash,
            audioStream->ssrcOutboundContexts,
            audioStream->localSsrc,
            audioStream->rtpMap,
            audioStream->telephoneEventRtpMap);

        if (ssrcContext)
        {
            audioStream->transport.getJobQueue().addJob<EncodeJob>(std::move(audioPacket),
                *ssrcContext,
                audioStream->transport,
                _rtpTimestampSource);
        }
    }
}

void EngineMixer::markAssociatedAudioOutboundContextsForDeletion(const EngineAudioStream* senderAudioStream)
{
    if (!senderAudioStream->remoteSsrc.isSet())
    {
        return;
    }

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto* audioStream = audioStreamEntry.second;
        if (audioStream == senderAudioStream || audioStream->mediaMode == MediaMode::SSRC_REWRITE)
        {
            // If we use ssrc rewrite there is no need to remove the outbound context as it is not there
            continue;
        }
        const auto endpointIdHash = audioStreamEntry.first;

        auto outboundContextItr = audioStream->ssrcOutboundContexts.find(senderAudioStream->remoteSsrc.get());
        if (outboundContextItr != audioStream->ssrcOutboundContexts.end())
        {
            outboundContextItr->second.markedForDeletion = true;
            logger::info("Marking unused audio outbound context for deletion, ssrc %u, endpointIdHash %lu",
                _loggableId.c_str(),
                outboundContextItr->first,
                endpointIdHash);

            static constexpr uint32_t feedbackSsrc = 0;
            audioStream->transport.getJobQueue().addJob<FinalizeNonSsrcRewriteOutboundContextJob>(*this,
                audioStream->transport,
                outboundContextItr->second,
                _engineSyncContext,
                _messageListener,
                feedbackSsrc);
        }
    }
}

bool EngineMixer::asyncRemoveStream(const EngineAudioStream* engineAudioStream)
{
    return post([=]() { this->removeStream(engineAudioStream); });
}

bool EngineMixer::asyncReconfigureAudioStream(const transport::RtcTransport& transport, const uint32_t remoteSsrc)
{
    return post(utils::bind(&EngineMixer::reconfigureAudioStream, this, std::cref(transport), remoteSsrc));
}

bool EngineMixer::asyncReconfigureNeighbours(const transport::RtcTransport& transport,
    const std::vector<uint32_t>& neighbours)
{
    return post(utils::bind(&EngineMixer::reconfigureNeighbours, this, std::cref(transport), neighbours));
}

bool EngineMixer::asyncAddAudioStream(EngineAudioStream* engineAudioStream)
{
    return post(utils::bind(&EngineMixer::addAudioStream, this, engineAudioStream));
}

} // namespace bridge
