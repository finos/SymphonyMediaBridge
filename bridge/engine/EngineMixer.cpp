#include "bridge/engine/EngineMixer.h"
#include "api/DataChannelMessage.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/VideoNackReceiveJob.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"

using namespace bridge;

namespace bridge
{

EngineMixer::EngineMixer(const std::string& id,
    jobmanager::JobManager& jobManager,
    const concurrency::SynchronizationContext& engineSyncContext,
    jobmanager::JobManager& backgroundJobQueue,
    MixerManagerAsync& messageListener,
    const uint32_t localVideoSsrc,
    const config::Config& config,
    memory::PacketPoolAllocator& sendAllocator,
    memory::AudioPacketPoolAllocator& audioAllocator,
    memory::PacketPoolAllocator& mainAllocator,
    const std::vector<uint32_t>& audioSsrcs,
    const std::vector<api::SimulcastGroup>& videoSsrcs,
    const uint32_t lastN)
    : _id(id),
      _loggableId("EngineMixer"),
      _jobManager(jobManager),
      _engineSyncContext(engineSyncContext),
      _messageListener(messageListener),
      _incomingBarbellSctp(128),
      _incomingForwarderAudioRtp(maxPendingPackets),
      _incomingRtcp(maxPendingRtcpPackets),
      _incomingForwarderVideoRtp(maxPendingPackets),
      _engineAudioStreams(maxStreamsPerModality),
      _engineVideoStreams(maxStreamsPerModality),
      _engineDataStreams(maxStreamsPerModality),
      _engineRecordingStreams(maxRecordingStreams),
      _engineBarbells(maxNumBarbells),
      _neighbourMemberships(ActiveMediaList::maxParticipants),
      _ssrcInboundContexts(maxSsrcs),
      _allSsrcInboundContexts(maxSsrcs),
      _audioSsrcToUserIdMap(ActiveMediaList::maxParticipants),
      _localVideoSsrc(localVideoSsrc),
      _rtpTimestampSource(1000),
      _mainAllocator(mainAllocator),
      _sendAllocator(sendAllocator),
      _audioAllocator(audioAllocator),
      _lastStartedIterationTimestamp(utils::Time::getAbsoluteTime()),
      _lastReceiveTimeOnRegularTransports(_lastStartedIterationTimestamp),
      _lastReceiveTimeOnBarbellTransports(_lastStartedIterationTimestamp),
      _lastSendTimeOfUserMediaMapMessageOverBarbells(_lastStartedIterationTimestamp),
      _lastCounterCheck(0),
      _engineStreamDirector(std::make_unique<EngineStreamDirector>(_loggableId.getInstanceId(), config, lastN)),
      _activeMediaList(std::make_unique<ActiveMediaList>(_loggableId.getInstanceId(),
          audioSsrcs,
          videoSsrcs,
          lastN,
          config.audio.lastN,
          config.audio.activeTalkerSilenceThresholdDb)),
      _lastUplinkEstimateUpdate(0),
      _lastIdleTransportCheck(0),
      _config(config),
      _lastN(lastN),
      _numMixedAudioStreams(0),
      _lastVideoBandwidthCheck(0),
      _lastVideoPacketProcessed(0),
      _lastTickJobStartTimestamp(0),
      _hasSentTimeout(false),
      _probingVideoStreams(false),
      _minUplinkEstimate(0),
      _backgroundJobQueue(backgroundJobQueue),
      _lastRecordingAckProcessed(_lastStartedIterationTimestamp),
      _slidesPresent(false)
{
    assert(audioSsrcs.size() <= SsrcRewrite::ssrcArraySize);
    assert(videoSsrcs.size() <= SsrcRewrite::ssrcArraySize);

    std::memset(_mixedData, 0, sizeof(_mixedData));
    _iceReceivedOnRegularTransport.test_and_set();
    _iceReceivedOnBarbellTransport.test_and_set();
}

EngineMixer::~EngineMixer() {}

bool EngineMixer::isIdle(const uint64_t timestamp) const
{
    if (!utils::Time::diffGE(_lastReceiveTimeOnRegularTransports,
            timestamp,
            _config.mixerInactivityTimeoutMs * utils::Time::ms))
    {
        return false;
    }

    return _config.deleteEmptyConferencesWithBarbells ||
        utils::Time::diffGE(_lastReceiveTimeOnBarbellTransports,
            timestamp,
            _config.mixerInactivityTimeoutMs * utils::Time::ms);
}

memory::UniquePacket EngineMixer::createGoodBye(uint32_t ssrc, memory::PacketPoolAllocator& allocator)
{
    auto packet = memory::makeUniquePacket(allocator);
    if (packet)
    {
        auto* rtcp = rtp::RtcpGoodbye::create(packet->get(), ssrc);
        packet->setLength(rtcp->header.size());
    }
    return packet;
}

void EngineMixer::addDataSteam(EngineDataStream* engineDataStream)
{
    const auto endpointIdHash = engineDataStream->endpointIdHash;
    if (_engineDataStreams.contains(endpointIdHash))
    {
        return;
    }

    logger::debug("Add engineDataStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineDataStream->transport.getLoggableId().c_str(),
        endpointIdHash);

    _engineDataStreams.emplace(endpointIdHash, engineDataStream);
}

void EngineMixer::removeStream(const EngineDataStream* engineDataStream)
{
    const auto endpointIdHash = engineDataStream->endpointIdHash;
    logger::debug("Remove engineDataStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineDataStream->transport.getLoggableId().c_str(),
        endpointIdHash);

    _engineDataStreams.erase(endpointIdHash);

    engineDataStream->transport.postOnQueue(
        [this, engineDataStream]() { _messageListener.asyncDataStreamRemoved(*this, *engineDataStream); });
}

void EngineMixer::startTransport(transport::RtcTransport& transport)
{
    logger::debug("Starting transport %s", transport.getLoggableId().c_str(), _loggableId.c_str());
    transport.setDataReceiver(this);

    // start on transport allows incoming packets.
    // Postponing this until callbacks has been set and remote ice and dtls has been configured is vital to avoid
    // race condition and sync problems with ice session and srtpclient
    transport.start();
    transport.connect();
}

void EngineMixer::clear()
{
    for (auto& engineAudioStream : _engineAudioStreams)
    {
        engineAudioStream.second->transport.setDataReceiver(nullptr);
    }
    _engineAudioStreams.clear();
}

void EngineMixer::flush()
{
    logger::debug("flushing media from RTP queues", _loggableId.c_str());
    _incomingForwarderAudioRtp.clear();
    _incomingForwarderVideoRtp.clear();
    _incomingRtcp.clear();
}

void EngineMixer::forwardPackets(const uint64_t engineTimestamp)
{
    processIncomingRtpPackets(engineTimestamp);
}

void EngineMixer::run(const uint64_t engineIterationStartTimestamp)
{
    _rtpTimestampSource += framesPerIteration1kHz;
    _lastStartedIterationTimestamp = engineIterationStartTimestamp;

    // 1. Process all incoming packets
    processBarbellSctp(engineIterationStartTimestamp);
    processIncomingRtpPackets(engineIterationStartTimestamp);
    processIncomingRtcpPackets(engineIterationStartTimestamp);
    processIceActivity(engineIterationStartTimestamp);

    // 2. Check for stale streams
    checkPacketCounters(engineIterationStartTimestamp);

    runDominantSpeakerCheck(engineIterationStartTimestamp);
    sendMessagesToNewDataStreams();
    markSsrcsInUse(engineIterationStartTimestamp);
    processMissingPackets(engineIterationStartTimestamp); // must run after checkPacketCounters

    sendPeriodicUserMediaMapMessageOverBarbells(engineIterationStartTimestamp);

    // 3. Update bandwidth estimates
    if (_config.rctl.useUplinkEstimate)
    {
        checkIfRateControlIsNeeded(engineIterationStartTimestamp);
        updateDirectorUplinkEstimates(engineIterationStartTimestamp);
        checkVideoBandwidth(engineIterationStartTimestamp);
    }

    // 4. Perform audio mixing
    if (_rtpTimestampSource % 20 == 0)
    {
        processAudioStreams();
    }

    // 5. Check if Transports are alive
    removeIdleStreams(engineIterationStartTimestamp);

    // 6. Maintain transports.
    runTransportTicks(engineIterationStartTimestamp);

    if (!_hasSentTimeout && isIdle(engineIterationStartTimestamp))
    {
        _hasSentTimeout = _messageListener.asyncMixerTimedOut(*this);
    }
}

void EngineMixer::runDominantSpeakerCheck(const uint64_t engineIterationStartTimestamp)
{
    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;
    bool audioMapChanged = false;
    auto mapRevision = _activeMediaList->getMapRevision();
    _activeMediaList->process(engineIterationStartTimestamp, dominantSpeakerChanged, videoMapChanged, audioMapChanged);

    if (dominantSpeakerChanged)
    {
        sendDominantSpeakerMessageToAll();
        sendLastNListMessageToAll();
    }

    if (videoMapChanged)
    {
        sendUserMediaMapMessageToAll();
    }
    if (mapRevision != _activeMediaList->getMapRevision())
    {
        sendUserMediaMapMessageOverBarbells();
    }
}

void EngineMixer::markSsrcsInUse(const uint64_t timestamp)
{
    for (auto& it : _ssrcInboundContexts)
    {
        auto inboundContext = it.second;
        if (inboundContext->rtpMap.isAudio())
        {
            inboundContext->isSsrcUsed = _activeMediaList->isInActiveTalkerList(inboundContext->endpointIdHash);
            continue;
        }

        const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(inboundContext->endpointIdHash);
        const auto previousUse = inboundContext->isSsrcUsed.load();

        inboundContext->isSsrcUsed = _engineStreamDirector->isSsrcUsed(inboundContext->ssrc,
            inboundContext->endpointIdHash,
            inboundContext->hasRecentActivity(utils::Time::sec, timestamp),
            isSenderInLastNList,
            _engineRecordingStreams.size());

        if (previousUse != inboundContext->isSsrcUsed)
        {
            logger::debug("ssrc %u changed in use state to %c, pinned %c",
                _loggableId.c_str(),
                inboundContext->ssrc,
                previousUse ? 'f' : 't',
                _engineStreamDirector->isPinned(inboundContext->endpointIdHash) ? 't' : 'f');
        }
    }
}

void EngineMixer::updateDirectorUplinkEstimates(const uint64_t engineIterationStartTimestamp)
{
    if (utils::Time::diffLT(_lastUplinkEstimateUpdate, engineIterationStartTimestamp, 1ULL * utils::Time::sec))
    {
        return;
    }
    _lastUplinkEstimateUpdate = engineIterationStartTimestamp;

    for (const auto& videoStreamEntry : _engineVideoStreams)
    {
        if (!videoStreamEntry.second->transport.isConnected())
        {
            continue;
        }

        auto* videoStream = videoStreamEntry.second;
        const auto uplinkEstimateKbps = videoStream->transport.getUplinkEstimateKbps();
        if (uplinkEstimateKbps == 0)
        {
            continue;
        }
        _engineStreamDirector->setUplinkEstimateKbps(videoStream->endpointIdHash,
            uplinkEstimateKbps,
            engineIterationStartTimestamp);
    }
}

void EngineMixer::internalRemoveInboundSsrc(uint32_t ssrc)
{
    if (_ssrcInboundContexts.contains(ssrc))
    {
        assert(false);
        logger::error("trying to remove inbound ssrc context that is still in use %u", _loggableId.c_str(), ssrc);
        return;
    }

    auto* context = _allSsrcInboundContexts.getItem(ssrc);
    if (context)
    {
        logger::info("Removing inbound context ssrc %u", _loggableId.c_str(), ssrc);

        auto decoder = context->opusDecoder.release();
        _allSsrcInboundContexts.erase(ssrc);
        if (decoder)
        {
            _messageListener.asyncInboundSsrcContextRemoved(*this, ssrc, decoder);
        }
    }
}

void EngineMixer::checkPacketCounters(const uint64_t timestamp)
{
    if (_lastCounterCheck != 0 && utils::Time::diffLT(_lastCounterCheck, timestamp, utils::Time::sec))
    {
        return;
    }
    _lastCounterCheck = timestamp;
    checkInboundPacketCounters(timestamp);
    checkRecordingPacketCounters(timestamp);
    checkBarbellPacketCounters(timestamp);
}

void EngineMixer::checkInboundPacketCounters(const uint64_t timestamp)
{
    for (auto& inboundContextEntry : _ssrcInboundContexts)
    {
        auto& inboundContext = *inboundContextEntry.second;

        if (!inboundContext.activeMedia)
        {
            continue; // it will turn active on next packet arrival
        }

        if (inboundContext.rtpMap.isAudio() && inboundContext.sender)
        {
            if (inboundContext.hasRecentActivity(utils::Time::sec, timestamp))
            {
                inboundContext.sender->postOnQueue([&inboundContext]() {
                    inboundContext.opusDecodePacketRate =
                        inboundContext.opusPacketRate ? inboundContext.opusPacketRate->get() : 0;
                });
            }
            else
            {
                inboundContext.activeMedia = false;
                inboundContext.opusDecodePacketRate = 0;
            }
        }

        const auto endpointIdHash = inboundContext.endpointIdHash.load();
        const bool recentActivity =
            inboundContext.hasRecentActivity(_config.idleInbound.transitionTimeout * utils::Time::ms, timestamp);
        const auto ssrc = inboundContextEntry.first;
        auto receiveCounters = inboundContext.sender->getCumulativeReceiveCounters(ssrc);

        if (!recentActivity && receiveCounters.packets > 5 && inboundContext.activeMedia &&
            inboundContext.rtpMap.format != RtpMap::Format::RTX)
        {
            inboundContext.activeMedia = false;
            _engineStreamDirector->streamActiveStateChanged(endpointIdHash, ssrc, false);

            if (inboundContext.rtpMap.isVideo() && _engineVideoStreams.contains(endpointIdHash))
            {
                inboundContext.inactiveTransitionCount++;

                // The reason for checking if the ssrc is equal to inboundContext.rewriteSsrc, is because that is
                // the default simulcast level. We don't want to drop the default level even if it's unstable.
                if (inboundContext.inactiveTransitionCount >= _config.idleInbound.dropAfterIdleTransitions.get() &&
                    ssrc != inboundContext.defaultLevelSsrc)
                {
                    logger::info("Inbound packets ssrc %u, transport %s, endpointIdHash %lu will be dropped",
                        _loggableId.c_str(),
                        inboundContext.ssrc,
                        inboundContext.sender->getLoggableId().c_str(),
                        endpointIdHash);

                    inboundContext.shouldDropPackets = true;
                }
            }
        }

        if (!inboundContext.hasRecentActivity(_config.idleInbound.decommissionTimeout * utils::Time::sec, timestamp))
        {
            if (inboundContext.rtpMap.format == RtpMap::Format::VP8)
            {
                auto videoStream = _engineVideoStreams.getItem(endpointIdHash);
                if (videoStream)
                {
                    auto rtxSsrc = videoStream->getFeedbackSsrcFor(inboundContext.ssrc);
                    if (rtxSsrc.isSet())
                    {
                        decommissionInboundContext(rtxSsrc.get());
                    }
                    decommissionInboundContext(inboundContext.ssrc);
                }
            }
            else if (inboundContext.rtpMap.format != RtpMap::Format::RTX)
            {
                logger::info("Inbound context ssrc %u has been idle for 5 minutes", _loggableId.c_str(), ssrc);
                decommissionInboundContext(inboundContext.ssrc);
            }
        }
    }
}

EngineStats::MixerStats EngineMixer::gatherStats(const uint64_t iterationStartTime)
{
    EngineStats::MixerStats stats;
    uint64_t idleTimestamp = iterationStartTime - utils::Time::sec * 2;

    stats.audioInQueues = 0;
    stats.audioInQueueSamples = 0;
    stats.maxAudioInQueueSamples = 0;

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        const auto* audioStream = audioStreamEntry.second;
        const auto audioRecvCounters = audioStream->transport.getAudioReceiveCounters(idleTimestamp);
        const auto audioSendCounters = audioStream->transport.getAudioSendCounters(idleTimestamp);
        const auto videoRecvCounters = audioStream->transport.getVideoReceiveCounters(idleTimestamp);
        const auto videoSendCounters = audioStream->transport.getVideoSendCounters(idleTimestamp);
        const auto pacingQueueCount = audioStream->transport.getPacingQueueCount();
        const auto rtxPacingQueueCount = audioStream->transport.getRtxPacingQueueCount();

        stats.inbound.audio += audioRecvCounters;
        stats.outbound.audio += audioSendCounters;
        stats.inbound.video += videoRecvCounters;
        stats.outbound.video += videoSendCounters;
        stats.inbound.transport.addBandwidthGroup(audioStream->transport.getDownlinkEstimateKbps());
        stats.inbound.transport.addRttGroup(audioStream->transport.getRtt() / utils::Time::ms);
        stats.inbound.transport.addLossGroup((audioRecvCounters + videoRecvCounters).getReceiveLossRatio());
        stats.outbound.transport.addLossGroup((audioSendCounters + videoSendCounters).getSendLossRatio());
        stats.pacingQueue += pacingQueueCount;
        stats.rtxPacingQueue += rtxPacingQueueCount;

        if (audioStream->detectedAudioSsrc.isSet())
        {
            auto inboundSsrcContext = _ssrcInboundContexts.getItem(audioStream->detectedAudioSsrc.get());
            if (inboundSsrcContext)
            {
                stats.opusDecodePacketsPerSecond += inboundSsrcContext->opusDecodePacketRate.load();
                stats.audioLevelExtensionStreamCount += inboundSsrcContext->hasAudioLevelExtension.load() ? 1 : 0;
            }
        }

        if (_numMixedAudioStreams > 0 && audioStream->remoteSsrc.isSet())
        {
            auto* ssrcContext = _ssrcInboundContexts.getItem(audioStream->remoteSsrc.get());
            if (ssrcContext && ssrcContext->hasAudioReceivePipe.load())
            {
                ++stats.audioInQueues;
                // fetching JB sizes would require more atomics in audio pipeline.
            }
        }
    }

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        if (!_engineAudioStreams.contains(videoStreamEntry.first))
        {
            const auto videoRecvCounters = videoStreamEntry.second->transport.getVideoReceiveCounters(idleTimestamp);
            const auto videoSendCounters = videoStreamEntry.second->transport.getVideoSendCounters(idleTimestamp);
            const auto pacingQueueCount = videoStreamEntry.second->transport.getPacingQueueCount();
            const auto rtxPacingQueueCount = videoStreamEntry.second->transport.getRtxPacingQueueCount();

            stats.inbound.video += videoRecvCounters;
            stats.outbound.video += videoSendCounters;
            stats.inbound.transport.addBandwidthGroup(videoStreamEntry.second->transport.getDownlinkEstimateKbps());
            stats.inbound.transport.addRttGroup(videoStreamEntry.second->transport.getRtt() / utils::Time::ms);
            stats.inbound.transport.addLossGroup(videoRecvCounters.getReceiveLossRatio());
            stats.outbound.transport.addLossGroup(videoSendCounters.getSendLossRatio());
            stats.pacingQueue += pacingQueueCount;
            stats.rtxPacingQueue += rtxPacingQueueCount;
        }
    }

    return stats;
}

void EngineMixer::onConnected(transport::RtcTransport* sender)
{
    logger::debug("transport connected", sender->getLoggableId().c_str());

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        auto videoStream = videoStreamEntry.second;
        if (&videoStream->transport == sender)
        {
            continue;
        }
    }

    auto barbell = _engineBarbells.getItem(sender->getEndpointIdHash());
    if (barbell && barbell->transport.isDtlsClient())
    {
        barbell->transport.connectSctp();
    }
}

void EngineMixer::handleSctpControl(const size_t endpointIdHash, memory::UniquePacket packet)
{
    auto& header = webrtc::streamMessageHeader(*packet);
    auto* dataStream = _engineDataStreams.getItem(endpointIdHash);
    if (dataStream)
    {
        const bool wasOpen = dataStream->stream.isOpen();
        dataStream->stream.onSctpMessage(&dataStream->transport,
            header.id,
            header.sequenceNumber,
            header.payloadProtocol,
            header.data(),
            packet->getLength() - sizeof(header));

        if (!wasOpen && dataStream->stream.isOpen())
        {
            sendUserMediaMapMessage(endpointIdHash);
        }
    }
    else
    {
        logger::warn("Data stream not found to handle sctp control for endpoint %lu",
            _loggableId.c_str(),
            endpointIdHash);
    }
}

void EngineMixer::sendEndpointMessage(const size_t toEndpointIdHash,
    const size_t fromEndpointIdHash,
    memory::UniqueAudioPacket packet)
{
    if (!fromEndpointIdHash || !packet)
    {
        assert(false);
        return;
    }

    auto* fromDataStream = _engineDataStreams.getItem(fromEndpointIdHash);
    if (!fromDataStream)
    {
        return;
    }

    auto message = reinterpret_cast<const char*>(packet->get());
    utils::StringBuilder<2048> endpointMessage;

    if (toEndpointIdHash)
    {
        auto* toDataStream = _engineDataStreams.getItem(toEndpointIdHash);
        if (!toDataStream || !toDataStream->stream.isOpen())
        {
            return;
        }

        api::DataChannelMessage::makeEndpointMessage(endpointMessage,
            toDataStream->endpointId,
            fromDataStream->endpointId,
            message);

        toDataStream->stream.sendString(endpointMessage.get(), endpointMessage.getLength());
        logger::debug("Endpoint message %lu -> %lu: %s",
            _loggableId.c_str(),
            fromEndpointIdHash,
            toEndpointIdHash,
            endpointMessage.get());
    }
    else
    {
        logger::debug("Broadcast Endpoint message from %lu %s",
            _loggableId.c_str(),
            fromEndpointIdHash,
            endpointMessage.get());
        for (auto& dataStreamEntry : _engineDataStreams)
        {
            if (dataStreamEntry.first == fromEndpointIdHash || !dataStreamEntry.second->stream.isOpen())
            {
                continue;
            }

            endpointMessage.clear();
            api::DataChannelMessage::makeEndpointMessage(endpointMessage,
                dataStreamEntry.second->endpointId,
                fromDataStream->endpointId,
                message);

            dataStreamEntry.second->stream.sendString(endpointMessage.get(), endpointMessage.getLength());
        }
    }
}

bool EngineMixer::onSctpConnectionRequest(transport::RtcTransport* sender, uint16_t remotePort)
{
    logger::debug("SCTP connect request", sender->getLoggableId().c_str());
    return true;
}

void EngineMixer::onSctpEstablished(transport::RtcTransport* sender)
{
    logger::info("SCTP association established %s", _loggableId.c_str(), sender->getLoggableId().c_str());

    auto barbell = _engineBarbells.getItem(sender->getEndpointIdHash());
    if (barbell && sender->isDtlsClient())
    {
        logger::debug("opening barbell webrtc data channel on %s",
            _loggableId.c_str(),
            sender->getLoggableId().c_str());
        barbell->dataChannel.open("barbell");
    }
}

void EngineMixer::onSctpMessage(transport::RtcTransport* sender,
    uint16_t streamId,
    uint16_t streamSequenceNumber,
    uint32_t payloadProtocol,
    const void* data,
    size_t length)
{
    assert(sender);
    if (EngineBarbell::isFromBarbell(sender->getTag()))
    {
        auto packet = webrtc::makeUniquePacket(streamId, payloadProtocol, data, length, _sendAllocator);
        _incomingBarbellSctp.push(IncomingPacketInfo(std::move(packet), sender));
        return;
    }

    auto packet = webrtc::makeUniquePacket(streamId, payloadProtocol, data, length, _sendAllocator);
    if (!packet)
    {
        logger::error("Unable to allocate sctp message, sender %p, length %lu", _loggableId.c_str(), sender, length);
        return;
    }

    _messageListener.asyncSctpReceived(*this, packet, sender->getEndpointIdHash());
}

/**
 * We store source endpointIdHash in the Packet itself, because, after arriving here, the packet goes through
 * decryption and is ready to be forwarded. At that time we want to know how this source endpoint is mapped to
 * outbound ssrc and thus need the source endpointIdHash. The source ssrc is insufficient as another endpoint may
 * have taken its place by then.
 */
void EngineMixer::onRtpPacketReceived(transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        return;
    }
    const uint32_t ssrc = rtpHeader->ssrc;

    auto ssrcContext = emplaceInboundSsrcContext(ssrc, sender, rtpHeader->payloadType, timestamp);
    if (!ssrcContext)
    {
        logger::warn("could not acquire inbound ssrc context ssrc %u", _loggableId.c_str(), ssrc);
        return;
    }

    ssrcContext->onRtpPacketReceived(timestamp);

    if (EngineBarbell::isFromBarbell(sender->getTag()))
    {
        const bool isAudio = ssrcContext->rtpMap.isAudio();
        if (!setPacketSourceEndpointIdHash(*packet, sender->getEndpointIdHash(), ssrc, isAudio))
        {
            logger::debug("incoming barbell packet unmapped %zu ssrc %u %s",
                _loggableId.c_str(),
                sender->getEndpointIdHash(),
                ssrc,
                isAudio ? "audio" : "video");
            return; // drop packet as we cannot process it
        }
        ssrcContext->endpointIdHash = packet->endpointIdHash;
    }

    // telephone event is a special case as the inbound context is the same of the audio codec.
    if (!ssrcContext->telephoneEventRtpMap.isEmpty() &&
        ssrcContext->telephoneEventRtpMap.payloadType == rtpHeader->payloadType)
    {
        onAudioTelephoneEventRtpPacketReceived(*ssrcContext,
            sender,
            std::move(packet),
            extendedSequenceNumber,
            timestamp);
        return;
    }

    switch (ssrcContext->rtpMap.format)
    {
    case bridge::RtpMap::Format::OPUS:
        onAudioRtpPacketReceived(*ssrcContext, sender, std::move(packet), extendedSequenceNumber, timestamp);
        break;
    case bridge::RtpMap::Format::VP8:
    case bridge::RtpMap::Format::H264:
        onVideoRtpPacketReceived(*ssrcContext, sender, std::move(packet), extendedSequenceNumber, timestamp);
        break;
    case bridge::RtpMap::Format::RTX:
        onVideoRtpRtxPacketReceived(*ssrcContext, sender, std::move(packet), extendedSequenceNumber, timestamp);
        break;
    default:
        logger::warn("Unexpected payload format %d onRtpPacketReceived",
            getLoggableId().c_str(),
            rtpHeader->payloadType);
        break;
    }
}

void EngineMixer::onRtcpPacketDecoded(transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    assert(packet);
    if (!_incomingRtcp.push(IncomingPacketInfo(std::move(packet), sender, 0)))
    {
        logger::warn("rtcp queue full", _loggableId.c_str());
    }
}

SsrcOutboundContext* EngineMixer::obtainOutboundSsrcContext(size_t endpointIdHash,
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext>& ssrcOutboundContexts,
    const uint32_t ssrc,
    const RtpMap& rtpMap,
    const RtpMap& telephoneEventRtpMap)
{
    auto* ssrcOutboundContext = ssrcOutboundContexts.getItem(ssrc);
    if (ssrcOutboundContext)
    {
        return ssrcOutboundContext;
    }

    auto emplaceResult = ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, rtpMap, telephoneEventRtpMap);

    if (!emplaceResult.second && emplaceResult.first == ssrcOutboundContexts.end())
    {
        logger::error("Failed to create %s outbound context for ssrc %u, endpointIdHash %zu",
            _loggableId.c_str(),
            rtpMap.isAudio() ? "audio" : "video",
            ssrc,
            endpointIdHash);
        return nullptr;
    }

    logger::info("Created new %s outbound context for stream, endpointIdHash %zu, ssrc %u",
        _loggableId.c_str(),
        rtpMap.isAudio() ? "audio" : "video",
        endpointIdHash,
        ssrc);

    return &emplaceResult.first->second;
}

SsrcOutboundContext* EngineMixer::obtainOutboundForwardSsrcContext(size_t endpointIdHash,
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext>& ssrcOutboundContexts,
    const uint32_t ssrc,
    const RtpMap& rtpMap,
    const RtpMap& telephoneEventRtpMap)
{
    auto ssrcOutboundContext = ssrcOutboundContexts.getItem(ssrc);
    if (ssrcOutboundContext)
    {
        if (ssrcOutboundContext->markedForDeletion)
        {
            return nullptr;
        }

        return ssrcOutboundContext;
    }

    // In this case most likely both input stream and ssrcOutboundContext have been removed already
    // and we are processing an old packet. If we create an new ssrcOutboundContext it would be leaked
    // until the end of outbound stream
    const bool isInboundDecommissioned = !_ssrcInboundContexts.contains(ssrc);
    if (isInboundDecommissioned)
    {
        return nullptr;
    }

    auto emplaceResult = ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, rtpMap, telephoneEventRtpMap);

    if (!emplaceResult.second && emplaceResult.first == ssrcOutboundContexts.end())
    {
        logger::error("Failed to create outbound context for ssrc %u, endpointIdHash %zu",
            _loggableId.c_str(),
            ssrc,
            endpointIdHash);
        return nullptr;
    }

    logger::info("Created new outbound context for stream, endpointIdHash %zu, ssrc %u",
        _loggableId.c_str(),
        endpointIdHash,
        ssrc);
    return &emplaceResult.first->second;
}

void EngineMixer::onOutboundContextFinalized(size_t ownerEndpointHash,
    uint32_t ssrc,
    uint32_t feedbackSsrc,
    bool isVideo)
{
    if (isVideo)
    {
        auto* videoStream = _engineVideoStreams.getItem(ownerEndpointHash);
        if (videoStream)
        {
            videoStream->ssrcOutboundContexts.erase(ssrc);
            if (feedbackSsrc != 0)
            {
                videoStream->ssrcOutboundContexts.erase(feedbackSsrc);
            }
        }

        return;
    }

    assert(feedbackSsrc == 0);
    auto* audioStream = _engineAudioStreams.getItem(ownerEndpointHash);
    if (audioStream)
    {
        audioStream->ssrcOutboundContexts.erase(ssrc);
    }
}

SsrcInboundContext* EngineMixer::emplaceInboundSsrcContext(const uint32_t ssrc,
    transport::RtcTransport* sender,
    const uint32_t payloadType,
    const uint64_t timestamp)
{
    {
        auto ssrcInboundContext = _ssrcInboundContexts.getItem(ssrc);
        if (ssrcInboundContext)
        {
            return ssrcInboundContext;
        }
    }

    if (_allSsrcInboundContexts.contains(ssrc))
    {
        // being removed
        return nullptr;
    }

    const auto endpointIdHash = sender->getEndpointIdHash();

    if (EngineBarbell::isFromBarbell(sender->getTag()))
    {
        emplaceBarbellInboundSsrcContext(ssrc, sender, payloadType, timestamp);
    }

    auto* audioStream = _engineAudioStreams.getItem(endpointIdHash);
    if (audioStream &&
        (audioStream->rtpMap.payloadType == payloadType ||
            audioStream->telephoneEventRtpMap.payloadType == payloadType))
    {
        if (!audioStream->remoteSsrc.isSet())
        {
            audioStream->remoteSsrc.set(ssrc);
            logger::info("picking up inbound audio on ssrc %u automatically", _loggableId.c_str(), ssrc);
        }
        else if (audioStream->remoteSsrc.get() != ssrc)
        {
            return nullptr;
        }

        auto* rtpMap = &audioStream->rtpMap;
        bridge::RtpMap rtpMapCopy;
        if (!rtpMap->audioLevelExtId.isSet())
        {
            auto id = rtpMap->suggestAudioLevelExtensionId();
            if (id != RtpMap::ExtHeaderIdentifiers::EOL)
            {
                rtpMapCopy = *rtpMap;
                rtpMapCopy.audioLevelExtId.set(id);
                rtpMap = &rtpMapCopy;
            }
        }

        auto emplaceResult =
            _allSsrcInboundContexts.emplace(ssrc, ssrc, *rtpMap, audioStream->telephoneEventRtpMap, sender, timestamp);

        if (!emplaceResult.second && emplaceResult.first == _allSsrcInboundContexts.end())
        {
            logger::error("Failed to create inbound context for ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }

        auto emplaceIt = _ssrcInboundContexts.emplace(ssrc, &emplaceResult.first->second);
        if (emplaceIt.second)
        {
            emplaceIt.first->second->endpointIdHash = endpointIdHash;
        }

        logger::info("Created new inbound context for audio stream ssrc %u, endpointIdHash %lu, audioLevelExtId %d, "
                     "absSendTimeExtId %d, %s",
            _loggableId.c_str(),
            ssrc,
            audioStream->endpointIdHash,
            audioStream->rtpMap.audioLevelExtId.isSet() ? audioStream->rtpMap.audioLevelExtId.get() : -1,
            audioStream->rtpMap.absSendTimeExtId.isSet() ? audioStream->rtpMap.absSendTimeExtId.get() : -1,
            sender->getLoggableId().c_str());
        return &emplaceResult.first->second;
    }

    const auto videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!videoStream)
    {
        return nullptr;
    }

    if (payloadType == videoStream->rtpMap.payloadType)
    {
        uint32_t defaultLevelSsrc = 0;
        auto level = videoStream->simulcastStream.getLevelOf(ssrc);
        if (level.isSet())
        {
            defaultLevelSsrc = videoStream->simulcastStream.levels[0].ssrc;
        }
        else if (videoStream->secondarySimulcastStream.isSet())
        {
            level = videoStream->secondarySimulcastStream.get().getLevelOf(ssrc);
            if (level.isSet())
            {
                defaultLevelSsrc = videoStream->secondarySimulcastStream.get().levels[0].ssrc;
            }
        }

        if (!level.isSet())
        {
            return nullptr;
        }

        auto emplaceResult =
            _allSsrcInboundContexts
                .emplace(ssrc, ssrc, videoStream->rtpMap, sender, timestamp, level.get(), defaultLevelSsrc);
        if (!emplaceResult.second && emplaceResult.first == _allSsrcInboundContexts.end())
        {
            logger::error("failed to create inbound ssrc context for video. ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }

        auto& inboundContext = emplaceResult.first->second;

        auto emplaceIt = _ssrcInboundContexts.emplace(ssrc, &emplaceResult.first->second);
        if (emplaceIt.second)
        {
            emplaceIt.first->second->endpointIdHash = endpointIdHash;
        }

        logger::info("Created new inbound context for video stream ssrc %u, level %u, rewrite ssrc %u, "
                     "endpointIdHash %lu, rtp "
                     "format %u, %s",
            _loggableId.c_str(),
            ssrc,
            level.get(),
            inboundContext.defaultLevelSsrc,
            videoStream->endpointIdHash,
            static_cast<uint16_t>(inboundContext.rtpMap.format),
            sender->getLoggableId().c_str());

        return &inboundContext;
    }
    else if (payloadType == videoStream->feedbackRtpMap.payloadType)
    {
        auto emplaceResult =
            _allSsrcInboundContexts.emplace(ssrc, ssrc, videoStream->feedbackRtpMap, sender, timestamp, 0, 0);
        if (!emplaceResult.second && emplaceResult.first == _allSsrcInboundContexts.end())
        {
            logger::error("failed to create inbound ssrc context for video rtx. ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }

        auto& inboundContext = emplaceResult.first->second;

        auto emplaceIt = _ssrcInboundContexts.emplace(ssrc, &emplaceResult.first->second);
        if (emplaceIt.second)
        {
            emplaceIt.first->second->endpointIdHash = endpointIdHash;
        }
        logger::debug(
            "Created new inbound context for video stream feedback ssrc %u, endpointIdHash %lu, rtp format %u, %s",
            _loggableId.c_str(),
            ssrc,
            videoStream->endpointIdHash,
            static_cast<uint16_t>(inboundContext.rtpMap.format),
            sender->getLoggableId().c_str());

        return &inboundContext;
    }

    return nullptr;
}

void EngineMixer::processIncomingRtpPackets(const uint64_t timestamp)
{
    uint32_t numRtpPackets = 0;
    uint32_t numBarbellRtpPackets = 0;

    if (_numMixedAudioStreams > 0)
    {
        for (auto& audioStream : _engineAudioStreams)
        {
            if (audioStream.second->remoteSsrc.isSet())
            {
                auto ssrcContext = _ssrcInboundContexts.getItem(audioStream.second->remoteSsrc.get());
                if (ssrcContext && ssrcContext->hasAudioReceivePipe.load() &&
                    ssrcContext->audioReceivePipe->needProcess())
                {
                    auto& transport = audioStream.second->transport;
                    transport.postOnQueue(
                        [ssrcContext]() { ssrcContext->audioReceivePipe->process(utils::Time::getAbsoluteTime()); });
                }
            }
        }
    }

    for (IncomingPacketInfo packetInfo; _incomingForwarderAudioRtp.pop(packetInfo);)
    {
        ++numRtpPackets;
        if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()))
        {
            ++numBarbellRtpPackets;
        }

        const auto rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
        if (!rtpHeader)
        {
            continue;
        }

        auto ssrcContext = packetInfo.inboundContext();
        if (ssrcContext && !ssrcContext->activeMedia)
        {
            ssrcContext->activeMedia = true;
        }

        forwardAudioRtpPacket(packetInfo, timestamp);
        forwardAudioRtpPacketRecording(packetInfo, timestamp);
        forwardAudioRtpPacketOverBarbell(packetInfo, timestamp);
    }

    for (IncomingPacketInfo packetInfo; _incomingForwarderVideoRtp.pop(packetInfo);)
    {
        ++numRtpPackets;
        if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()))
        {
            ++numBarbellRtpPackets;
        }

        auto ssrcContext = packetInfo.inboundContext();
        if (ssrcContext && !ssrcContext->activeMedia)
        {
            _engineStreamDirector->streamActiveStateChanged(packetInfo.packet()->endpointIdHash,
                ssrcContext->ssrc,
                true);
            ssrcContext->activeMedia = true;
        }

        forwardVideoRtpPacket(packetInfo, timestamp);
        forwardVideoRtpPacketOverBarbell(packetInfo, timestamp);
        forwardVideoRtpPacketRecording(packetInfo, timestamp);
    }

    if (numBarbellRtpPackets > 0)
    {
        _lastReceiveTimeOnBarbellTransports = timestamp;
    }

    if (numBarbellRtpPackets != numRtpPackets)
    {
        _lastReceiveTimeOnRegularTransports = timestamp;
    }
}

void EngineMixer::processIncomingRtcpPackets(const uint64_t timestamp)
{
    for (IncomingPacketInfo packetInfo; _incomingRtcp.pop(packetInfo);)
    {
        rtp::CompoundRtcpPacket compoundPacket(packetInfo.packet()->get(), packetInfo.packet()->getLength());
        for (const auto& rtcpPacket : compoundPacket)
        {
            switch (rtcpPacket.packetType)
            {
            case rtp::RtcpPacketType::RECEIVER_REPORT:
            case rtp::RtcpPacketType::SENDER_REPORT:
                break;
            case rtp::RtcpPacketType::PAYLOADSPECIFIC_FB:
                processIncomingPayloadSpecificRtcpPacket(packetInfo.transport()->getEndpointIdHash(),
                    rtcpPacket,
                    timestamp);
                break;
            case rtp::RtcpPacketType::RTPTRANSPORT_FB:
                processIncomingTransportFbRtcpPacket(packetInfo.transport(), rtcpPacket, timestamp);
                break;
            default:
                break;
            }
        }

        if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()))
        {
            _lastReceiveTimeOnBarbellTransports = timestamp;
        }
        else
        {
            _lastReceiveTimeOnRegularTransports = timestamp;
        }
    }
}

void EngineMixer::processIceActivity(const uint64_t timestamp)
{
    if (!_iceReceivedOnBarbellTransport.test_and_set())
    {
        _lastReceiveTimeOnBarbellTransports = timestamp;
    }

    if (!_iceReceivedOnRegularTransport.test_and_set())
    {
        _lastReceiveTimeOnRegularTransports = timestamp;
    }
}

void EngineMixer::processIncomingPayloadSpecificRtcpPacket(const size_t rtcpSenderEndpointIdHash,
    const rtp::RtcpHeader& rtcpPacket,
    const uint64_t timestamp)
{
    auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
    if (rtcpFeedback->header.fmtCount != rtp::PayloadSpecificFeedbackType::Pli &&
        rtcpFeedback->header.fmtCount != rtp::PayloadSpecificFeedbackType::Fir)
    {
        return;
    }

    const auto mediaSsrc = rtcpFeedback->mediaSsrc.get();
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext>* outboundContexts = nullptr;
    auto* rtcpSenderVideoStream = _engineVideoStreams.getItem(rtcpSenderEndpointIdHash);
    if (rtcpSenderVideoStream)
    {
        outboundContexts = &rtcpSenderVideoStream->ssrcOutboundContexts;
    }
    else
    {
        auto* barbell = _engineBarbells.getItem(rtcpSenderEndpointIdHash);
        if (barbell)
        {
            outboundContexts = &barbell->ssrcOutboundContexts;
        }
    }

    const auto* outboundContext = (outboundContexts ? outboundContexts->getItem(mediaSsrc) : nullptr);
    if (!outboundContext)
    {
        logger::warn("cannot find outbound context for PLI request RTCP feedback from %zu to ssrc %u",
            _loggableId.c_str(),
            rtcpSenderEndpointIdHash,
            mediaSsrc);
        return;
    }

    auto* inboundContext = _ssrcInboundContexts.getItem(outboundContext->getOriginalSsrc());
    if (inboundContext)
    {
        logger::info("PLI request handled from %zu on %u",
            _loggableId.c_str(),
            rtcpSenderEndpointIdHash,
            inboundContext->ssrc);
        inboundContext->pliScheduler.triggerPli();
    }
}

void EngineMixer::processIncomingTransportFbRtcpPacket(const transport::RtcTransport* transport,
    const rtp::RtcpHeader& rtcpPacket,
    const uint64_t timestamp)
{
    auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
    if (rtcpFeedback->header.fmtCount != rtp::TransportLayerFeedbackType::PacketNack)
    {
        return;
    }

    const auto fromBarbell = EngineBarbell::isFromBarbell(transport->getTag());
    const auto mediaSsrc = rtcpFeedback->mediaSsrc.get();

    if (fromBarbell)
    {
        const auto barbell = _engineBarbells.getItem(transport->getEndpointIdHash());
        if (barbell)
        {
            processIncomingBarbellFbRtcpPacket(*barbell, *rtcpFeedback, timestamp);
        }
        return;
    }

    auto* rtcpSenderVideoStream = _engineVideoStreams.getItem(transport->getEndpointIdHash());
    if (!rtcpSenderVideoStream)
    {
        return;
    }

    auto* mediaSsrcOutboundContext = rtcpSenderVideoStream->ssrcOutboundContexts.getItem(mediaSsrc);
    if (!mediaSsrcOutboundContext)
    {
        return;
    }

    uint32_t feedbackSsrc;
    const bool ssrcRewrite = rtcpSenderVideoStream->ssrcRewrite;
    if (!(ssrcRewrite ? _activeMediaList->getFeedbackSsrc(mediaSsrc, feedbackSsrc)
                      : _engineStreamDirector->getFeedbackSsrc(mediaSsrc, feedbackSsrc)))
    {
        return;
    }

    SsrcOutboundContext* rtxSsrcOutboundContext = nullptr;
    if (ssrcRewrite)
    {
        rtxSsrcOutboundContext = obtainOutboundSsrcContext(rtcpSenderVideoStream->endpointIdHash,
            rtcpSenderVideoStream->ssrcOutboundContexts,
            feedbackSsrc,
            rtcpSenderVideoStream->feedbackRtpMap,
            bridge::RtpMap::EMPTY);
    }
    else
    {
        rtxSsrcOutboundContext = obtainOutboundForwardSsrcContext(rtcpSenderVideoStream->endpointIdHash,
            rtcpSenderVideoStream->ssrcOutboundContexts,
            feedbackSsrc,
            rtcpSenderVideoStream->feedbackRtpMap,
            bridge::RtpMap::EMPTY);
    }

    if (!rtxSsrcOutboundContext)
    {
        return;
    }

    mediaSsrcOutboundContext->onRtpSent(timestamp);
    const auto numFeedbackControlInfos = rtp::getNumFeedbackControlInfos(rtcpFeedback);
    uint16_t pid = 0;
    uint16_t blp = 0;
    logger::debug("Client NACK for %zu pkts on ssrc %u, %s",
        _loggableId.c_str(),
        numFeedbackControlInfos,
        mediaSsrc,
        transport->getLoggableId().c_str());
    for (size_t i = 0; i < numFeedbackControlInfos; ++i)
    {
        rtxSsrcOutboundContext->onRtpSent(timestamp);
        rtp::getFeedbackControlInfo(rtcpFeedback, i, numFeedbackControlInfos, pid, blp);
        rtcpSenderVideoStream->transport.getJobQueue().addJob<bridge::VideoNackReceiveJob>(*rtxSsrcOutboundContext,
            rtcpSenderVideoStream->transport,
            *mediaSsrcOutboundContext,
            pid,
            blp,
            timestamp,
            transport->getRtt());
    }
}

void EngineMixer::sendLastNListMessage(const size_t endpointIdHash)
{
    utils::StringBuilder<1024> lastNListMessage;
    auto* dataStream = _engineDataStreams.getItem(endpointIdHash);
    if (!dataStream)
    {
        return;
    }
    if (!dataStream->stream.isOpen() || !dataStream->hasSeenInitialSpeakerList)
    {
        return;
    }

    const auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!videoStream || videoStream->ssrcRewrite)
    {
        return;
    }

    auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
    _activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage);

    dataStream->stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
}

void EngineMixer::sendLastNListMessageToAll()
{
    utils::StringBuilder<1024> lastNListMessage;

    for (auto& dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (!dataStream->stream.isOpen() || !dataStream->hasSeenInitialSpeakerList)
        {
            continue;
        }

        const auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
        if (!videoStream || videoStream->ssrcRewrite)
        {
            continue;
        }

        lastNListMessage.clear();
        auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
        _activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage);

        dataStream->stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
    }
}

void EngineMixer::sendMessagesToNewDataStreams()
{
    bool isDominantSpeakerMessageBuild = false;
    utils::StringBuilder<256> dominantSpeakerMessage;
    utils::StringBuilder<1024> lastNListMessage;
    utils::StringBuilder<1024> userMediaMapMessage;

    for (auto& dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (dataStream->hasSeenInitialSpeakerList || !dataStream->stream.isOpen())
        {
            continue;
        }

        if (!isDominantSpeakerMessageBuild)
        {
            _activeMediaList->makeDominantSpeakerMessage(dominantSpeakerMessage);
            isDominantSpeakerMessageBuild = true;
        }

        if (!dominantSpeakerMessage.empty())
        {
            dataStream->stream.sendString(dominantSpeakerMessage.get(), dominantSpeakerMessage.getLength());
        }

        auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
        if (!videoStream)
        {
            dataStream->hasSeenInitialSpeakerList = true;
            continue;
        }
        const auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);

        if (videoStream->ssrcRewrite)
        {
            userMediaMapMessage.clear();
            if (_activeMediaList->makeUserMediaMapMessage(_lastN,
                    dataStreamEntry.first,
                    pinTarget,
                    _engineVideoStreams,
                    userMediaMapMessage))
            {
                dataStream->stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
            }
        }
        else
        {
            lastNListMessage.clear();
            if (_activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage))
            {
                dataStream->stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
            }
        }

        dataStream->hasSeenInitialSpeakerList = true;
    }
}

void EngineMixer::updateBandwidthFloor()
{
    auto videoStreams = 0;
    for (const auto& videoStreamEntry : _engineVideoStreams)
    {
        if (videoStreamEntry.second->simulcastStream.numLevels > 0)
        {
            ++videoStreams;
        }
        if (videoStreamEntry.second->secondarySimulcastStream.isSet() &&
            videoStreamEntry.second->secondarySimulcastStream.get().numLevels > 0)
        {
            ++videoStreams;
        }
    }

    auto audioStreams = 0;
    for (const auto& audioStreamEntry : _engineAudioStreams)
    {
        if (audioStreamEntry.second->remoteSsrc.isSet())
        {
            ++audioStreams;
        }
    }

    _engineStreamDirector->updateBandwidthFloor(_lastN, audioStreams, videoStreams);
}

void EngineMixer::sendUserMediaMapMessage(const size_t endpointIdHash)
{
    utils::StringBuilder<1024> userMediaMapMessage;

    auto* dataStream = _engineDataStreams.getItem(endpointIdHash);
    if (!dataStream)
    {
        return;
    }

    if (!dataStream->stream.isOpen() || !dataStream->hasSeenInitialSpeakerList)
    {
        return;
    }

    const auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (!videoStream || !videoStream->ssrcRewrite)
    {
        return;
    }

    const auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
    _activeMediaList->makeUserMediaMapMessage(_lastN,
        endpointIdHash,
        pinTarget,
        _engineVideoStreams,
        userMediaMapMessage);

    dataStream->stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
}

void EngineMixer::sendUserMediaMapMessageToAll()
{
    utils::StringBuilder<1024> userMediaMapMessage;
    for (auto dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (!dataStream->stream.isOpen() || !dataStream->hasSeenInitialSpeakerList)
        {
            continue;
        }

        const auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
        if (!videoStream || !videoStream->ssrcRewrite)
        {
            continue;
        }

        userMediaMapMessage.clear();
        const auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
        _activeMediaList->makeUserMediaMapMessage(_lastN,
            endpointIdHash,
            pinTarget,
            _engineVideoStreams,
            userMediaMapMessage);

        dataStream->stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
    }
}

void EngineMixer::sendDominantSpeakerMessageToAll()
{
    utils::StringBuilder<256> dominantSpeakerMessage;
    _activeMediaList->makeDominantSpeakerMessage(dominantSpeakerMessage);

    for (auto dataStreamEntry : _engineDataStreams)
    {
        auto dataStream = dataStreamEntry.second;

        if (!dataStream->stream.isOpen() || !dataStream->hasSeenInitialSpeakerList)
        {
            continue;
        }
        dataStream->stream.sendString(dominantSpeakerMessage.get(), dominantSpeakerMessage.getLength());
    }

    const auto dominantSpeaker = _activeMediaList->getDominantSpeaker();
    auto* dataStream = _engineDataStreams.getItem(dominantSpeaker);
    if (dataStream)
    {
        for (auto& recStreamPair : _engineRecordingStreams)
        {
            sendDominantSpeakerToRecordingStream(*recStreamPair.second, dominantSpeaker, dataStream->endpointId);
        }
    }
}

void EngineMixer::decommissionInboundContext(const uint32_t ssrc)
{
    auto* ssrcContext = _ssrcInboundContexts.getItem(ssrc);
    if (ssrcContext)
    {
        _ssrcInboundContexts.erase(ssrc); // remove first or internalRemoveInboundSsrc may assert
        ssrcContext->sender->postOnQueue(utils::bind(&EngineMixer::internalRemoveInboundSsrc, this, ssrc));
        logger::info("Decommissioned inbound ssrc context %u", _loggableId.c_str(), ssrc);
    }
}

bool EngineMixer::needToUpdateMinUplinkEstimate(const uint32_t curEstimate, const uint32_t oldEstimate) const
{
    // For screen sharing (a.k.a. 'slides') we have a minimum allowed bitrate of 900 kbps,
    // so it is not worth to react on the fluctuation below ~10% of this value.
    return abs((int64_t)oldEstimate - (int64_t)curEstimate) > 100;
}

uint32_t EngineMixer::getMinRemoteClientDownlinkBandwidth() const
{
    auto minBitrate = 100000u;
    for (const auto& itBb : _engineBarbells)
    {
        minBitrate = std::min(minBitrate, itBb.second->minClientDownlinkBandwidth);
    }
    return minBitrate;
}

void EngineMixer::checkIfRateControlIsNeeded(const uint64_t timestamp)
{
    auto enableBEProbing = isVideoInUse(timestamp, utils::Time::sec * _config.rctl.cooldownInterval);

    if (enableBEProbing == _probingVideoStreams)
        return;

    for (auto& videoStream : _engineVideoStreams)
    {
        if (videoStream.second)
        {
            if (enableBEProbing)
            {
                startProbingVideoStream(*videoStream.second);
            }
            else
            {
                stopProbingVideoStream(*videoStream.second);
            }
        }
    }

    _probingVideoStreams = enableBEProbing;
}

template <typename TStream>
void removeIdleStreams(concurrency::MpmcHashmap32<size_t, TStream*>& streams,
    EngineMixer* mixer,
    const uint64_t timestamp)
{
    for (const auto& it : streams)
    {
        auto stream = it.second;
        if (!stream->idleTimeoutSeconds)
        {
            continue;
        }

        if (utils::Time::diffLT(stream->createdAt, timestamp, stream->idleTimeoutSeconds * utils::Time::sec))
        {
            continue;
        }

        const auto lastReceivedTimestamp = stream->transport.getLastReceivedPacketTimestamp();
        if (utils::Time::diffGE(lastReceivedTimestamp, timestamp, stream->idleTimeoutSeconds * utils::Time::sec))
        {
            logger::info("Removing idle stream for endpoint %zu, last received packet timestamp: %" PRIu64
                         ", current: %" PRIu64,
                "EngineMixer",
                stream->endpointIdHash,
                lastReceivedTimestamp,
                timestamp);
            mixer->removeStream(stream);
        }
    }
}

void EngineMixer::removeIdleStreams(const uint64_t timestamp)
{
    if (utils::Time::diffLT(_lastIdleTransportCheck, timestamp, 1ULL * utils::Time::sec))
    {
        return;
    }
    _lastIdleTransportCheck = timestamp;

    ::bridge::removeIdleStreams<EngineVideoStream>(_engineVideoStreams, this, timestamp);
    ::bridge::removeIdleStreams<EngineAudioStream>(_engineAudioStreams, this, timestamp);
    ::bridge::removeIdleStreams<EngineDataStream>(_engineDataStreams, this, timestamp);
}

void EngineMixer::runTransportTicks(const uint64_t timestamp)
{
    if (utils::Time::diffLT(_lastTickJobStartTimestamp, timestamp, utils::Time::ms * 60))
    {
        return;
    }
    _lastTickJobStartTimestamp = timestamp;
    for (auto videoIt : _engineVideoStreams)
    {
        videoIt.second->transport.runTick(timestamp);
    }
}

size_t EngineMixer::getDominantSpeakerId() const
{
    return _activeMediaList->getDominantSpeaker();
}

std::map<size_t, ActiveTalker> EngineMixer::getActiveTalkers() const
{
    return _activeMediaList->getActiveTalkers();
}

utils::Optional<uint32_t> EngineMixer::getC9UserId(const size_t ssrc) const
{
    const auto it = _audioSsrcToUserIdMap.find(ssrc);
    if (it != _audioSsrcToUserIdMap.end())
    {
        return utils::Optional<uint32_t>(it->second);
    }
    return utils::Optional<uint32_t>();
}

void EngineMixer::mapSsrc2UserId(uint32_t ssrc, uint32_t usid)
{
    _audioSsrcToUserIdMap.emplace(ssrc, usid);
}

bool EngineMixer::asyncRemoveStream(const EngineDataStream* stream)
{
    return post([=]() { this->removeStream(stream); });
}

bool EngineMixer::asyncStartTransport(transport::RtcTransport& transport)
{
    return post(utils::bind(&EngineMixer::startTransport, this, std::ref(transport)));
}

bool EngineMixer::asyncAddDataSteam(EngineDataStream* engineDataStream)
{
    return post(utils::bind(&EngineMixer::addDataSteam, this, engineDataStream));
}

bool EngineMixer::asyncHandleSctpControl(const size_t endpointIdHash, memory::UniquePacket& packet)
{
    return post(utils::bind(&EngineMixer::handleSctpControl, this, endpointIdHash, utils::moveParam(packet)));
}

void EngineMixer::onIceReceived(transport::RtcTransport* transport, uint64_t timestamp)
{
    if (EngineBarbell::isFromBarbell(transport->getTag()))
    {
        _iceReceivedOnBarbellTransport.clear();
    }
    else
    {
        _iceReceivedOnRegularTransport.clear();
    }
}
} // namespace bridge
