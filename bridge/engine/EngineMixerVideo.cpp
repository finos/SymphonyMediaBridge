#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AddPacketCacheJob.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/DiscardReceivedVideoPacketJob.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/FinalizeNonSsrcRewriteOutboundContextJob.h"
#include "bridge/engine/ProcessMissingVideoPacketsJob.h"
#include "bridge/engine/RemovePacketCacheJob.h"
#include "bridge/engine/SendRtcpJob.h"
#include "bridge/engine/SetMaxMediaBitrateJob.h"
#include "bridge/engine/VideoForwarderReceiveJob.h"
#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/VideoNackReceiveJob.h"

namespace
{

/**
 * @return true if this ssrc should be skipped and not forwarded to the videoStream.
 */
inline bool shouldSkipBecauseOfWhitelist(const bridge::EngineVideoStream& videoStream, const uint32_t ssrc)
{
    if (!videoStream.ssrcWhitelist.enabled)
    {
        return false;
    }

    switch (videoStream.ssrcWhitelist.numSsrcs)
    {
    case 0:
        return true;
    case 1:
        return videoStream.ssrcWhitelist.ssrcs[0] != ssrc;
    case 2:
        return videoStream.ssrcWhitelist.ssrcs[0] != ssrc && videoStream.ssrcWhitelist.ssrcs[1] != ssrc;
    default:
        return false;
    }
}

} // namespace

namespace bridge
{

void EngineMixer::addVideoStream(EngineVideoStream* engineVideoStream)
{
    const auto endpointIdHash = engineVideoStream->endpointIdHash;
    if (_engineVideoStreams.contains(endpointIdHash))
    {
        return;
    }

    logger::debug("Add engineVideoStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineVideoStream->transport.getLoggableId().c_str(),
        endpointIdHash);

    if (_probingVideoStreams)
    {
        startProbingVideoStream(*engineVideoStream);
    }
    const auto mapRevision = _activeMediaList->getMapRevision();
    const auto it = _engineVideoStreams.emplace(endpointIdHash, engineVideoStream);
    if (!it.second)
    {
        logger::error("Emplace video stream has failed, transport %s, endpointIdHash %lu",
            _loggableId.c_str(),
            engineVideoStream->transport.getLoggableId().c_str(),
            endpointIdHash);
    }

    if (engineVideoStream->simulcastStream.numLevels > 0)
    {
        _engineStreamDirector->addParticipant(endpointIdHash, engineVideoStream->simulcastStream);
        _activeMediaList->addVideoParticipant(endpointIdHash,
            engineVideoStream->simulcastStream,
            engineVideoStream->secondarySimulcastStream,
            engineVideoStream->endpointId.c_str());
    }
    else
    {
        _engineStreamDirector->addParticipant(endpointIdHash);
    }
    updateBandwidthFloor();
    sendLastNListMessageToAll();
    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageToAll();
        sendUserMediaMapMessageOverBarbells();
    }

    sendVideoStreamToRecording(*engineVideoStream, true);
}

void EngineMixer::removeStream(const EngineVideoStream* engineVideoStream)
{
    engineVideoStream->transport.getJobQueue().getJobManager().abortTimedJob(engineVideoStream->transport.getId(),
        engineVideoStream->localSsrc);

    auto* outboundContext = engineVideoStream->ssrcOutboundContexts.getItem(engineVideoStream->localSsrc);
    if (outboundContext)
    {
        outboundContext->markedForDeletion = true;
        if (engineVideoStream->transport.isConnected())
        {
            stopProbingVideoStream(*engineVideoStream);
            auto goodByePacket = createGoodBye(outboundContext->ssrc, outboundContext->allocator);
            if (goodByePacket)
            {
                engineVideoStream->transport.getJobQueue().addJob<SendRtcpJob>(std::move(goodByePacket),
                    engineVideoStream->transport);
            }
        }
    }

    for (auto& ssrcOutboundContextPair : engineVideoStream->ssrcOutboundContexts)
    {
        if (!ssrcOutboundContextPair.second.markedForDeletion)
        {
            engineVideoStream->transport.getJobQueue().addJob<RemovePacketCacheJob>(*this,
                engineVideoStream->transport,
                ssrcOutboundContextPair.second,
                _messageListener);
        }
    }

    if (engineVideoStream->simulcastStream.numLevels != 0)
    {
        for (auto& simulcastLevel : engineVideoStream->simulcastStream.getLevels())
        {
            decommissionInboundContext(simulcastLevel.ssrc);
            decommissionInboundContext(simulcastLevel.feedbackSsrc);
        }

        markAssociatedVideoOutboundContextsForDeletion(engineVideoStream,
            engineVideoStream->simulcastStream.levels[0].ssrc,
            engineVideoStream->simulcastStream.levels[0].feedbackSsrc);
    }

    if (engineVideoStream->secondarySimulcastStream.isSet() &&
        engineVideoStream->secondarySimulcastStream.get().numLevels != 0)
    {
        for (auto& simulcastLevel : engineVideoStream->secondarySimulcastStream.get().getLevels())
        {
            decommissionInboundContext(simulcastLevel.ssrc);
            decommissionInboundContext(simulcastLevel.feedbackSsrc);
        }

        markAssociatedVideoOutboundContextsForDeletion(engineVideoStream,
            engineVideoStream->secondarySimulcastStream.get().levels[0].ssrc,
            engineVideoStream->secondarySimulcastStream.get().levels[0].feedbackSsrc);
    }

    const auto endpointIdHash = engineVideoStream->endpointIdHash;

    const auto mapRevision = _activeMediaList->getMapRevision();
    _activeMediaList->removeVideoParticipant(endpointIdHash);

    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageToAll();
        sendUserMediaMapMessageOverBarbells();
    }

    _engineStreamDirector->removeParticipant(endpointIdHash);
    _engineStreamDirector->removeParticipantPins(endpointIdHash);
    updateBandwidthFloor();

    sendVideoStreamToRecording(*engineVideoStream, false);

    logger::debug("Remove engineVideoStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineVideoStream->transport.getLoggableId().c_str(),
        endpointIdHash);

    const bool streamFound = _engineVideoStreams.erase(endpointIdHash);
    if (!streamFound)
    {
        logger::error("engineVideoStream has not been found, transport %s, endpointIdHash %lu",
            _loggableId.c_str(),
            engineVideoStream->transport.getLoggableId().c_str(),
            endpointIdHash);
    }

    engineVideoStream->transport.postOnQueue(
        [this, engineVideoStream]() { _messageListener.asyncVideoStreamRemoved(*this, *engineVideoStream); });
}

void EngineMixer::addVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* videoPacketCache)
{
    transport::RtcTransport* transport = nullptr;
    bridge::SsrcOutboundContext* ssrcContext = nullptr;
    auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (videoStream)
    {
        ssrcContext = videoStream->ssrcOutboundContexts.getItem(ssrc);
        transport = &videoStream->transport;
    }
    else
    {
        auto* barbell = _engineBarbells.getItem(endpointIdHash);
        if (barbell)
        {
            ssrcContext = barbell->ssrcOutboundContexts.getItem(ssrc);
            transport = &barbell->transport;
        }
    }

    if (ssrcContext && transport)
    {
        if (!transport->getJobQueue().addJob<AddPacketCacheJob>(*transport, *ssrcContext, videoPacketCache))
        {
            logger::warn("JobQueue full. Failed to add packet cache to %zu, ssrc %u, %s",
                _loggableId.c_str(),
                endpointIdHash,
                ssrc,
                transport->getLoggableId().c_str());
        }
    }
}

void EngineMixer::reconfigureVideoStream(const transport::RtcTransport& transport,
    const SsrcWhitelist& ssrcWhitelist,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream)
{
    const auto endpointIdHash = transport.getEndpointIdHash();

    auto* engineVideoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!engineVideoStream)
    {
        return;
    }

    logger::debug("Reconfigure %s video stream, endpointIdHash %lu, whitelist %c %u %u %u",
        _loggableId.c_str(),
        secondarySimulcastStream.isSet() ? "secondary" : "primary",
        transport.getEndpointIdHash(),
        ssrcWhitelist.enabled ? 't' : 'f',
        ssrcWhitelist.numSsrcs,
        ssrcWhitelist.ssrcs[0],
        ssrcWhitelist.ssrcs[1]);

    memory::Array<uint32_t, 12> decommissionedSsrcs;

    const auto mapRevision = _activeMediaList->getMapRevision();
    if (engineVideoStream->simulcastStream.numLevels != 0)
    {
        const bool hasLevelZeroChanged = simulcastStream.numLevels == 0 ||
            simulcastStream.levels[0].ssrc != engineVideoStream->simulcastStream.levels[0].ssrc;

        if (hasLevelZeroChanged)
        {
            removeVideoSsrcFromRecording(*engineVideoStream, engineVideoStream->simulcastStream.levels[0].ssrc);
        }

        for (auto& simulcastLevel : engineVideoStream->simulcastStream.getLevels())
        {
            decommissionedSsrcs.push_back(simulcastLevel.ssrc);
            decommissionedSsrcs.push_back(simulcastLevel.feedbackSsrc);
        }
    }

    if ((engineVideoStream->secondarySimulcastStream.isSet() &&
            engineVideoStream->secondarySimulcastStream.get().numLevels != 0))
    {
        const bool hasLevelZeroChanged = !secondarySimulcastStream.isSet() ||
            secondarySimulcastStream.get().levels[0].ssrc !=
                engineVideoStream->secondarySimulcastStream.get().levels[0].ssrc;

        if (hasLevelZeroChanged)
        {
            removeVideoSsrcFromRecording(*engineVideoStream,
                engineVideoStream->secondarySimulcastStream.get().levels[0].ssrc);
        }

        for (auto& simulcastLevel : engineVideoStream->secondarySimulcastStream.get().getLevels())
        {
            decommissionedSsrcs.push_back(simulcastLevel.ssrc);
            decommissionedSsrcs.push_back(simulcastLevel.feedbackSsrc);
        }
    }

    engineVideoStream->simulcastStream = simulcastStream;
    engineVideoStream->secondarySimulcastStream = secondarySimulcastStream;

    _engineStreamDirector->removeParticipant(endpointIdHash);
    _activeMediaList->removeVideoParticipant(endpointIdHash);

    auto decommissionedSsrcsEndIt = decommissionedSsrcs.end();

    if (engineVideoStream->simulcastStream.numLevels > 0)
    {
        if (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().numLevels > 0)
        {
            engineVideoStream->secondarySimulcastStream = secondarySimulcastStream;
            _engineStreamDirector->addParticipant(endpointIdHash,
                engineVideoStream->simulcastStream,
                &engineVideoStream->secondarySimulcastStream.get());

            for (auto& simulcastLevel : engineVideoStream->secondarySimulcastStream.get().getLevels())
            {
                decommissionedSsrcsEndIt =
                    std::remove(decommissionedSsrcs.begin(), decommissionedSsrcsEndIt, simulcastLevel.ssrc);
                decommissionedSsrcsEndIt =
                    std::remove(decommissionedSsrcs.begin(), decommissionedSsrcsEndIt, simulcastLevel.feedbackSsrc);
            }
        }
        else
        {
            _engineStreamDirector->addParticipant(endpointIdHash, engineVideoStream->simulcastStream);
        }

        for (auto& simulcastLevel : engineVideoStream->simulcastStream.getLevels())
        {
            decommissionedSsrcsEndIt =
                std::remove(decommissionedSsrcs.begin(), decommissionedSsrcsEndIt, simulcastLevel.ssrc);
            decommissionedSsrcsEndIt =
                std::remove(decommissionedSsrcs.begin(), decommissionedSsrcsEndIt, simulcastLevel.feedbackSsrc);
        }

        _activeMediaList->addVideoParticipant(endpointIdHash,
            engineVideoStream->simulcastStream,
            engineVideoStream->secondarySimulcastStream,
            engineVideoStream->endpointId.c_str());

        restoreDirectorStreamActiveState(*engineVideoStream, engineVideoStream->simulcastStream);
        if (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().numLevels > 0)
        {
            restoreDirectorStreamActiveState(*engineVideoStream, secondarySimulcastStream.get());
        }
    }
    else
    {
        _engineStreamDirector->addParticipant(endpointIdHash);
    }

    const auto decommissionedSpan = utils::Span<uint32_t>(decommissionedSsrcs.begin(), decommissionedSsrcsEndIt);
    for (const auto& decommissionedSsrc : decommissionedSpan)
    {
        decommissionInboundContext(decommissionedSsrc);
    }

    updateBandwidthFloor();
    sendLastNListMessageToAll();
    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageToAll();
        sendUserMediaMapMessageOverBarbells();
    }
    sendVideoStreamToRecording(*engineVideoStream, true);

    engineVideoStream->ssrcWhitelist = ssrcWhitelist;
}

void EngineMixer::processMissingPackets(const uint64_t timestamp)
{
    for (auto& ssrcInboundContextEntry : _ssrcInboundContexts)
    {
        auto& ssrcInboundContext = *ssrcInboundContextEntry.second;
        if (ssrcInboundContext.rtpMap.format != RtpMap::Format::VP8)
        {
            continue;
        }

        if (EngineBarbell::isFromBarbell(ssrcInboundContext.sender->getTag()))
        {
            processBarbellMissingPackets(ssrcInboundContext);
        }
        else
        {
            processEngineMissingPackets(ssrcInboundContext);
        }
    }

    processRecordingUnackedPackets(timestamp);
}

void EngineMixer::onVideoRtpPacketReceived(SsrcInboundContext& ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{

    if (ssrcContext.shouldDropPackets)
    {
        return;
    }

    const bool isFromBarbell = EngineBarbell::isFromBarbell(sender->getTag());
    const size_t endpointIdHash = ssrcContext.endpointIdHash;

    auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!videoStream && !isFromBarbell)
    {
        return;
    }

    const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(endpointIdHash);
    const bool mustBeForwardedOnBarbells = isSenderInLastNList && !_engineBarbells.empty() && !isFromBarbell;

    if (!mustBeForwardedOnBarbells && !ssrcContext.isSsrcUsed.load())
    {
        sender->getJobQueue().addJob<bridge::DiscardReceivedVideoPacketJob>(std::move(packet),
            sender,
            ssrcContext,
            extendedSequenceNumber,
            timestamp);
        return;
    }

    sender->getJobQueue().addJob<bridge::VideoForwarderReceiveJob>(std::move(packet),
        _sendAllocator,
        sender,
        *this,
        ssrcContext,
        _localVideoSsrc,
        extendedSequenceNumber,
        timestamp);
}

void EngineMixer::onVideoRtpRtxPacketReceived(SsrcInboundContext& rtxSsrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    const bool isFromBarbell = EngineBarbell::isFromBarbell(sender->getTag());
    const size_t endpointIdHash = rtxSsrcContext.endpointIdHash;
    EngineVideoStream* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!videoStream && !isFromBarbell)
    {
        return;
    }

    uint32_t mainSsrc;
    const uint32_t rtxSsrc = rtxSsrcContext.ssrc;
    if (videoStream && !_engineStreamDirector->getSsrc(videoStream->endpointIdHash, rtxSsrc, mainSsrc))
    {
        return;
    }
    else if (isFromBarbell)
    {
        auto ssrc = findBarbellMainSsrc(sender->getEndpointIdHash(), rtxSsrc);
        if (!ssrc.isSet())
        {
            return;
        }
        mainSsrc = ssrc.get();
    }

    auto mainSsrcContext = _ssrcInboundContexts.getItem(mainSsrc);
    if (!mainSsrcContext || mainSsrcContext->shouldDropPackets)
    {
        return;
    }

    mainSsrcContext->onRtpPacketReceived(timestamp);

    const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(endpointIdHash);
    const bool mustBeForwardedOnBarbells = isSenderInLastNList && !_engineBarbells.empty() && !isFromBarbell;

    // Optimization with isSsrcUsed is not need for barbell (barbell sends only necessary streams).
    if (videoStream && !mustBeForwardedOnBarbells && !mainSsrcContext->isSsrcUsed)
    {
        return;
    }

    sender->getJobQueue().addJob<bridge::VideoForwarderRtxReceiveJob>(std::move(packet),
        sender,
        *this,
        rtxSsrcContext,
        *mainSsrcContext,
        mainSsrc,
        extendedSequenceNumber);
}

void EngineMixer::pinEndpoint(const size_t endpointIdHash, const size_t targetEndpointIdHash)
{
    logger::debug("pinEndpoint %lu -> %lu", _loggableId.c_str(), endpointIdHash, targetEndpointIdHash);
    auto oldTarget = _engineStreamDirector->pin(endpointIdHash, targetEndpointIdHash);
    if (oldTarget == targetEndpointIdHash)
    {
        return;
    }

    auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (videoStream)
    {
        const auto oldPinSsrc = videoStream->pinSsrc;
        if (oldPinSsrc.isSet())
        {
            videoStream->videoPinSsrcs.push(oldPinSsrc.get());
            videoStream->pinSsrc.clear();
        }
        if (targetEndpointIdHash)
        {
            SimulcastLevel newPinSsrc;
            if (videoStream->videoPinSsrcs.pop(newPinSsrc))
            {
                videoStream->pinSsrc.set(newPinSsrc);
                logger::debug("EndpointIdHash %zu pin %zu, pin ssrc %u",
                    _loggableId.c_str(),
                    endpointIdHash,
                    targetEndpointIdHash,
                    newPinSsrc.ssrc);
            }
        }
    }

    sendLastNListMessage(endpointIdHash);
    sendUserMediaMapMessage(endpointIdHash);
}

void EngineMixer::onForwarderVideoRtpPacketDecrypted(SsrcInboundContext& inboundContext,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber)
{
    assert(packet);
    if (!_incomingForwarderVideoRtp.push(
            IncomingPacketInfo(std::move(packet), &inboundContext, extendedSequenceNumber)))
    {
        logger::error("Failed to push incoming forwarder video packet onto queue", getLoggableId().c_str());
        assert(false);
    }
}

void EngineMixer::forwardVideoRtpPacket(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
    if (!rtpHeader)
    {
        assert(false); // this should have been checked multiple times by now. Transport, ReceiveJob, RtxReceiveJob
        return;
    }

    const auto senderEndpointIdHash = packetInfo.packet()->endpointIdHash;

    _lastVideoPacketProcessed = timestamp;

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        const auto endpointIdHash = videoStreamEntry.first;
        auto videoStream = videoStreamEntry.second;
        if (!videoStream)
        {
            continue;
        }

        if (&videoStream->transport == packetInfo.transport())
        {
            continue;
        }

        if (!_engineStreamDirector->shouldForwardSsrc(endpointIdHash, packetInfo.inboundContext()->ssrc))
        {
            auto* senderVideoStream = _engineVideoStreams.getItem(senderEndpointIdHash);
            if (senderVideoStream)
            {
                bool fwdSomeLevel = false;
                for (auto& level : senderVideoStream->simulcastStream.getLevels())
                {
                    fwdSomeLevel |= _engineStreamDirector->shouldForwardSsrc(endpointIdHash, level.ssrc);
                }
                if (!fwdSomeLevel)
                {
                    logger::debug("no video is forwarded from %zu to %zu",
                        _loggableId.c_str(),
                        senderEndpointIdHash,
                        endpointIdHash);
                }
            }
            continue;
        }

        if (shouldSkipBecauseOfWhitelist(*videoStream, packetInfo.inboundContext()->ssrc))
        {
            continue;
        }

        uint32_t ssrc = 0;
        const bool ssrcRewrite = videoStream->ssrcRewrite;
        SsrcOutboundContext* ssrcOutboundContext = nullptr;
        if (ssrcRewrite)
        {
            const auto& screenShareSsrcMapping = _activeMediaList->getVideoScreenShareSsrcMapping();
            if (screenShareSsrcMapping.isSet() && screenShareSsrcMapping.get().first == senderEndpointIdHash &&
                screenShareSsrcMapping.get().second.ssrc == packetInfo.inboundContext()->ssrc)
            {
                ssrc = screenShareSsrcMapping.get().second.rewriteSsrc;
            }
            else if (_engineStreamDirector->getPinTarget(endpointIdHash) == senderEndpointIdHash &&
                !_activeMediaList->isInUserActiveVideoList(senderEndpointIdHash))
            {
                if (videoStream->pinSsrc.isSet())
                {
                    ssrc = videoStream->pinSsrc.get().ssrc;
                }
                else
                {
                    assert(false);
                    continue;
                }
            }
            else
            {
                const auto& videoSsrcRewriteMap = _activeMediaList->getVideoSsrcRewriteMap();
                const auto* rewriteMap = videoSsrcRewriteMap.getItem(senderEndpointIdHash);
                if (!rewriteMap)
                {
                    continue;
                }
                ssrc = (*rewriteMap)[0].main;
            }

            ssrcOutboundContext = obtainOutboundSsrcContext(videoStream->endpointIdHash,
                videoStream->ssrcOutboundContexts,
                ssrc,
                videoStream->rtpMap,
                bridge::RtpMap::EMPTY);
        }
        else
        {
            // non rewrite recipients gets the video sent on same ssrc as level 0.
            ssrc = packetInfo.inboundContext()->defaultLevelSsrc;
            ssrcOutboundContext = obtainOutboundForwardSsrcContext(videoStream->endpointIdHash,
                videoStream->ssrcOutboundContexts,
                ssrc,
                videoStream->rtpMap,
                bridge::RtpMap::EMPTY);
        }

        if (!ssrcOutboundContext)
        {
            continue;
        }

        if (!videoStream->transport.isConnected())
        {
            continue;
        }

        ssrcOutboundContext->onRtpSent(timestamp); // marks that we have active jobs on this ssrc context
        auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
        if (packet)
        {
            videoStream->transport.getJobQueue().addJob<VideoForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                *(packetInfo.inboundContext()),
                std::move(packet),
                videoStream->transport,
                packetInfo.extendedSequenceNumber(),
                _messageListener,
                videoStream->endpointIdHash,
                *this,
                timestamp);
        }
        else
        {
            logger::warn("send allocator depleted FwdRewrite", _loggableId.c_str());
        }
    }
}

// This method is called after a video stream has been reconfigured. That has led to streams being removed and then
// added again to ActiveMediaList and EngineStreamDirector. If the ssrc is an active inbound ssrc, we will set the
// stream state again in EngineStreamDirector.
void EngineMixer::restoreDirectorStreamActiveState(const EngineVideoStream& videoStream,
    const SimulcastStream& simulcastStream)
{
    for (auto& simulcastLevel : simulcastStream.getLevels())
    {
        const auto ssrc = simulcastLevel.ssrc;
        auto ssrcInboundContext = _ssrcInboundContexts.getItem(ssrc);
        if (ssrcInboundContext)
        {
            ssrcInboundContext->makeReady();
            _engineStreamDirector->streamActiveStateChanged(videoStream.endpointIdHash, ssrc, false);
        }
    }
}

void EngineMixer::markAssociatedVideoOutboundContextsForDeletion(const EngineVideoStream* senderVideoStream,
    const uint32_t ssrc,
    const uint32_t feedbackSsrc)
{
    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        auto* videoStream = videoStreamEntry.second;
        if (videoStream == senderVideoStream || videoStream->ssrcRewrite)
        {
            // If we use ssrc rewrite there is no need to remove the outbound context as it is not there
            continue;
        }
        const auto endpointIdHash = videoStreamEntry.first;

        auto* outboundContext = videoStream->ssrcOutboundContexts.getItem(ssrc);
        if (outboundContext)
        {
            outboundContext->markedForDeletion = true;
            logger::info("Marking unused video outbound context for deletion, ssrc %u, endpointIdHash %lu",
                _loggableId.c_str(),
                ssrc,
                endpointIdHash);

            videoStream->transport.getJobQueue().addJob<FinalizeNonSsrcRewriteOutboundContextJob>(*this,
                videoStream->transport,
                *outboundContext,
                _engineSyncContext,
                _messageListener,
                feedbackSsrc);
        }

        outboundContext = videoStream->ssrcOutboundContexts.getItem(feedbackSsrc);
        if (outboundContext)
        {
            outboundContext->markedForDeletion = true;
            logger::info("Marking unused video outbound context for deletion, feedback ssrc %u, endpointIdHash %lu´",
                _loggableId.c_str(),
                ssrc,
                endpointIdHash);
        }
    }
}

void EngineMixer::processEngineMissingPackets(bridge::SsrcInboundContext& ssrcInboundContext)
{
    auto* videoStream = _engineVideoStreams.getItem(ssrcInboundContext.sender->getEndpointIdHash());
    if (videoStream)
    {
        videoStream->transport.getJobQueue().addJob<bridge::ProcessMissingVideoPacketsJob>(ssrcInboundContext,
            videoStream->localSsrc,
            videoStream->transport,
            _sendAllocator);
    }
}

void EngineMixer::checkVideoBandwidth(const uint64_t timestamp)
{
    const bool isTimeToCheck = utils::Time::diffGE(_lastVideoBandwidthCheck, timestamp, utils::Time::sec * 3);
    const bool shouldExecute = isTimeToCheck || _engineStreamDirector->needsSlidesBitrateAllocation();
    if (!shouldExecute)
    {
        return;
    }

    _lastVideoBandwidthCheck = timestamp;
    uint32_t minUplinkEstimate = 10000000;
    bridge::SimulcastLevel* presenterSimulcastLevel = nullptr;
    bridge::EngineVideoStream* presenterStream = nullptr;
    for (auto videoIt : _engineVideoStreams)
    {
        auto& videoStream = *videoIt.second;
        if (videoStream.simulcastStream.contentType == SimulcastStream::VideoContentType::SLIDES)
        {
            presenterSimulcastLevel = &videoStream.simulcastStream.levels[0];
            presenterStream = videoIt.second;
        }
        else if (videoStream.secondarySimulcastStream.isSet() &&
            videoStream.secondarySimulcastStream.get().contentType == SimulcastStream::VideoContentType::SLIDES)
        {
            presenterSimulcastLevel = &videoStream.secondarySimulcastStream.get().levels[0];
            presenterStream = videoIt.second;
        }
        else
        {
            minUplinkEstimate = std::min(minUplinkEstimate, videoIt.second->transport.getUplinkEstimateKbps());
        }
    }

    minUplinkEstimate = std::max(minUplinkEstimate, _config.slides.minBitrate.get());
    if (needToUpdateMinUplinkEstimate(minUplinkEstimate, _minUplinkEstimate))
    {
        reportMinRemoteClientDownlinkBandwidthToBarbells(minUplinkEstimate);
    }

    minUplinkEstimate =
        std::max(_config.slides.minBitrate.get(), std::min(minUplinkEstimate, getMinRemoteClientDownlinkBandwidth()));

    // Local presenter. Sending TMBBR to restrict slides.
    if (presenterStream)
    {
        const uint32_t thumbnailsBitRate = _engineStreamDirector->getBitrateForAllThumbnails();
        uint32_t slidesLimit = minUplinkEstimate;
        if (minUplinkEstimate > _config.slides.borrowBandwidthThreshold.get())
        {
            // Reserve space for thumbnails and give a margin so not disable the thumbnails if
            // a participants starts to estimate slight lower bandwidth.
            // Never goes bellow the borrowBandwidthThreshold so we don't degrade slides too much
            slidesLimit = std::max(_config.slides.borrowBandwidthThreshold.get(),
                static_cast<uint32_t>(0.85 * (minUplinkEstimate - thumbnailsBitRate)));
        }

        logger::info("limiting slides bitrate for ssrc %u at %u, minUplinkEstimate %u, thumbnailsBitRate %u",
            _loggableId.c_str(),
            presenterSimulcastLevel->ssrc,
            slidesLimit,
            minUplinkEstimate,
            thumbnailsBitRate);

        presenterStream->transport.getJobQueue().addJob<SetMaxMediaBitrateJob>(presenterStream->transport,
            presenterStream->localSsrc,
            presenterSimulcastLevel->ssrc,
            slidesLimit,
            _sendAllocator);

        _engineStreamDirector->setSlidesSsrcAndBitrate(presenterSimulcastLevel->ssrc, slidesLimit);
    }
    else
    {
        _engineStreamDirector->setSlidesSsrcAndBitrate(0, 0);
    }

    // Any presenter (local or remote). Sending TMBBR to restrict video.
    const bool slidesPresent = _activeMediaList->getVideoScreenShareSsrcMapping().isSet();

    for (auto videoIt : _engineVideoStreams)
    {
        auto& videoStream = *videoIt.second;
        const uint32_t highQualityBitrate = slidesPresent ? 750 : 8000;

        if (3 == videoStream.simulcastStream.numLevels)
        {
            const auto& highResSsrc = videoStream.simulcastStream.levels[2].ssrc;
            logger::info("setting high-res bitrate for ssrc %u, at %u",
                _loggableId.c_str(),
                highResSsrc,
                highQualityBitrate);

            videoStream.transport.getJobQueue().addJob<SetMaxMediaBitrateJob>(videoStream.transport,
                videoStream.localSsrc,
                highResSsrc,
                highQualityBitrate,
                _sendAllocator);

            // WebRTC's incorrect implementation of TMBBR leads assigned bitrate to be
            // used for all members of the simulcast group, instead of the single layer.
            // That, in turn, leads to high res layer being switched on sometimes for a
            // short burst of frames. SFU detects constant switching on and off as "unstable"
            // behaviour and disables such layer forever. To prevent that side effect, we
            // reset "inactiveTransitionCount" when switching slides off.
            if (_slidesPresent && !slidesPresent)
            {
                auto inboundSsrcContext = _ssrcInboundContexts.getItem(highResSsrc);
                if (inboundSsrcContext)
                {
                    inboundSsrcContext->makeReady();
                }
            }
        }
    }
    _slidesPresent = slidesPresent;
}

void EngineMixer::startProbingVideoStream(EngineVideoStream& engineVideoStream)
{
    if (engineVideoStream.feedbackRtpMap.isEmpty())
    {
        return;
    }

    auto* outboundContext = obtainOutboundSsrcContext(engineVideoStream.endpointIdHash,
        engineVideoStream.ssrcOutboundContexts,
        engineVideoStream.localSsrc,
        engineVideoStream.feedbackRtpMap,
        bridge::RtpMap::EMPTY);

    if (outboundContext)
    {
        engineVideoStream.transport.postOnQueue(utils::bind(&transport::RtcTransport::setRtxProbeSource,
            &engineVideoStream.transport,
            engineVideoStream.localSsrc,
            &outboundContext->getSequenceNumberReference(),
            outboundContext->rtpMap.payloadType));
    }
    _probingVideoStreams = true;
}

void EngineMixer::stopProbingVideoStream(const EngineVideoStream& engineVideoStream)
{
    if (engineVideoStream.feedbackRtpMap.isEmpty())
    {
        return;
    }

    engineVideoStream.transport.postOnQueue(utils::bind(&transport::RtcTransport::setRtxProbeSource,
        &engineVideoStream.transport,
        engineVideoStream.localSsrc,
        nullptr,
        engineVideoStream.feedbackRtpMap.payloadType));
}

bool EngineMixer::isVideoInUse(const uint64_t timestamp, const uint64_t threshold) const
{
    return utils::Time::diffLE(_lastVideoPacketProcessed, timestamp, threshold);
}

bool EngineMixer::asyncReconfigureVideoStream(const transport::RtcTransport& transport,
    const SsrcWhitelist& ssrcWhitelist,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream)
{
    return post(utils::bind(&EngineMixer::reconfigureVideoStream,
        this,
        std::ref(transport),
        ssrcWhitelist,
        simulcastStream,
        secondarySimulcastStream));
}

bool EngineMixer::asyncRemoveStream(const EngineVideoStream* stream)
{
    return post([=]() { this->removeStream(stream); });
}

bool EngineMixer::asyncAddVideoPacketCache(const uint32_t ssrc,
    const size_t endpointIdHash,
    PacketCache* videoPacketCache)
{
    return post(utils::bind(&EngineMixer::addVideoPacketCache, this, ssrc, endpointIdHash, videoPacketCache));
}

bool EngineMixer::asyncAddVideoStream(EngineVideoStream* engineVideoStream)
{
    return post(utils::bind(&EngineMixer::addVideoStream, this, engineVideoStream));
}

bool EngineMixer::asyncPinEndpoint(const size_t endpointIdHash, const size_t targetEndpointIdHash)
{
    return post(utils::bind(&EngineMixer::pinEndpoint, this, endpointIdHash, targetEndpointIdHash));
}

bool EngineMixer::asyncSendEndpointMessage(const size_t toEndpointIdHash,
    const size_t fromEndpointIdHash,
    memory::UniqueAudioPacket& packet)
{
    return post(utils::bind(&EngineMixer::sendEndpointMessage,
        this,
        toEndpointIdHash,
        fromEndpointIdHash,
        utils::moveParam(packet)));
}
} // namespace bridge
