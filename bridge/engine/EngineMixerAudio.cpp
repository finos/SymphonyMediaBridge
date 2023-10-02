#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/FinalizeNonSsrcRewriteOutboundContextJob.h"
#include "bridge/engine/SendRtcpJob.h"
#include "codec/Opus.h"
#include "config/Config.h"

namespace
{
const int16_t mixSampleScaleFactor = 4;
}

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
    }

    logger::debug("Remove engineAudioStream, transport %s",
        _loggableId.c_str(),
        engineAudioStream->transport.getLoggableId().c_str());

    if (engineAudioStream->remoteSsrc.isSet())
    {
        decommissionInboundContext(engineAudioStream->remoteSsrc.get());
        _mixerSsrcAudioBuffers.erase(engineAudioStream->remoteSsrc.get());

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

void EngineMixer::addAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer)
{
    _mixerSsrcAudioBuffers.erase(ssrc);
    _mixerSsrcAudioBuffers.emplace(ssrc, audioBuffer);
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

void EngineMixer::onMixerAudioRtpPacketDecoded(SsrcInboundContext& inboundContext, memory::UniqueAudioPacket packet)
{
    assert(packet);
    if (!_incomingMixerAudioRtp.push(IncomingAudioPacketInfo(std::move(packet), &inboundContext)))
    {
        logger::error("Failed to push incoming mixer audio packet onto queue", getLoggableId().c_str());
        assert(false);
    }
}

namespace
{
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

} // namespace

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
                audioStream->rtpMap);
        }
        else
        {
            ssrcOutboundContext = obtainOutboundForwardSsrcContext(audioStream->endpointIdHash,
                audioStream->ssrcOutboundContexts,
                originalSsrc,
                audioStream->rtpMap);
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

/**
 * Append RTP audio to pre-buffer for this ssrc in _mixerSsrcAudioBuffers
 */
void EngineMixer::addPacketToMixerBuffers(const IncomingAudioPacketInfo& packetInfo,
    const uint64_t timestamp,
    bool overrunLogSpamGuard)
{
    const auto rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
    if (!rtpHeader)
    {
        return;
    }

    const auto ssrc = rtpHeader->ssrc;
    const auto sequenceNumber = rtpHeader->sequenceNumber;
    const auto payloadStart = rtpHeader->getPayload();
    const uint32_t payloadLength = packetInfo.packet()->getLength() - rtpHeader->headerLength();

    const auto mixerAudioBufferItr = _mixerSsrcAudioBuffers.find(ssrc.get());
    if (mixerAudioBufferItr == _mixerSsrcAudioBuffers.cend())
    {
        logger::debug("New ssrc %u seen, sequence %u, sending request to add audio buffer",
            _loggableId.c_str(),
            ssrc.get(),
            sequenceNumber.get());
        _mixerSsrcAudioBuffers.emplace(ssrc, nullptr);
        _messageListener.asyncAllocateAudioBuffer(*this, ssrc.get());
    }
    else if (!mixerAudioBufferItr->second)
    {
        logger::debug("new ssrc %u seen again, sequence %u, audio buffer is already requested",
            _loggableId.c_str(),
            ssrc.get(),
            sequenceNumber.get());
    }
    else
    {
        const auto samples = payloadLength / bytesPerSample;
        if (!mixerAudioBufferItr->second->write(reinterpret_cast<int16_t*>(payloadStart), samples))
        {
            if (!overrunLogSpamGuard)
            {
                logger::debug("Failed to write packet, buffer overrun, ssrc %u, sequence %u",
                    _loggableId.c_str(),
                    ssrc.get(),
                    sequenceNumber.get());
            }
            overrunLogSpamGuard = true;
        }
    }
}

void EngineMixer::mixSsrcBuffers()
{
    memset(_mixedData, 0, samplesPerIteration * codec::Opus::bytesPerSample);
    for (auto& mixerAudioBufferEntry : _mixerSsrcAudioBuffers)
    {
        if (!mixerAudioBufferEntry.second)
        {
            continue;
        }
        if (mixerAudioBufferEntry.second->isPreBuffering())
        {
            continue;
        }

        if (mixerAudioBufferEntry.second->getLength() < samplesPerIteration)
        {
            logger::debug("mixerAudioBufferEntry underrun", _loggableId.c_str());
            mixerAudioBufferEntry.second->setPreBuffering();
            continue;
        }
        else if (mixerAudioBufferEntry.second->getLength() < minimumSamplesInBuffer)
        {
            mixerAudioBufferEntry.second->insertSilence(samplesPerIteration);
        }

        mixerAudioBufferEntry.second->addToMix(_mixedData, samplesPerIteration, mixSampleScaleFactor);
    }
}

void EngineMixer::processAudioStreams()
{
    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto audioStream = audioStreamEntry.second;
        auto isContributingToMix = false;
        AudioBuffer* audioBuffer = nullptr;

        if (audioStream->remoteSsrc.isSet())
        {
            audioBuffer = _mixerSsrcAudioBuffers.getItem(audioStream->remoteSsrc.get());
            isContributingToMix = audioBuffer && !audioBuffer->isPreBuffering();

            if (!audioStream->isMixed() || !audioStream->transport.isConnected())
            {
                if (isContributingToMix)
                {
                    audioBuffer->drop(samplesPerIteration);
                }
                continue;
            }
        }

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

        auto payloadStart = rtpHeader->getPayload();
        const auto headerLength = rtpHeader->headerLength();
        audioPacket->setLength(headerLength + samplesPerIteration * bytesPerSample);
        memcpy(payloadStart, _mixedData, samplesPerIteration * bytesPerSample);

        if (isContributingToMix && audioBuffer)
        {
            audioBuffer->removeFromMix(reinterpret_cast<int16_t*>(payloadStart),
                samplesPerIteration,
                mixSampleScaleFactor);
            audioBuffer->drop(samplesPerIteration);
        }

        if (!audioStream->neighbours.empty())
        {
            for (auto& stream : _engineAudioStreams)
            {
                auto& peerAudioStream = *stream.second;
                if (stream.second->endpointIdHash != audioStream->endpointIdHash &&
                    peerAudioStream.remoteSsrc.isSet() && peerAudioStream.transport.isConnected() &&
                    areNeighbours(audioStream->neighbours, peerAudioStream.neighbours))
                {
                    auto* neighbourAudioBuffer = _mixerSsrcAudioBuffers.getItem(peerAudioStream.remoteSsrc.get());
                    if (!neighbourAudioBuffer || neighbourAudioBuffer->isPreBuffering())
                    {
                        continue;
                    }
                    neighbourAudioBuffer->removeFromMix(reinterpret_cast<int16_t*>(payloadStart),
                        samplesPerIteration,
                        mixSampleScaleFactor);
                }
            }
        }

        auto* ssrcContext = obtainOutboundSsrcContext(audioStream->endpointIdHash,
            audioStream->ssrcOutboundContexts,
            audioStream->localSsrc,
            audioStream->rtpMap);

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

bool EngineMixer::asyncAddAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer)
{
    return post(utils::bind(&EngineMixer::addAudioBuffer, this, ssrc, audioBuffer));
}

bool EngineMixer::asyncRemoveStream(const EngineAudioStream* engineAudioStream)
{
    return post([=]() { this->removeStream(engineAudioStream); });
}

bool EngineMixer::asyncReconfigureAudioStream(const transport::RtcTransport& transport, const uint32_t remoteSsrc)
{
    return post(utils::bind(&EngineMixer::reconfigureAudioStream, this, std::cref(transport), remoteSsrc));
}

bool EngineMixer::asyncAddAudioStream(EngineAudioStream* engineAudioStream)
{
    return post(utils::bind(&EngineMixer::addAudioStream, this, engineAudioStream));
}

} // namespace bridge
