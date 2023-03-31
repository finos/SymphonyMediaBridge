#include "bridge/engine/EngineMixer.h"
#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AddPacketCacheJob.h"
#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/DiscardReceivedVideoPacketJob.h"
#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/ProcessMissingVideoPacketsJob.h"
#include "bridge/engine/SendRtcpJob.h"
#include "bridge/engine/SetMaxMediaBitrateJob.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "bridge/engine/VideoForwarderReceiveJob.h"
#include "bridge/engine/VideoForwarderRewriteAndSendJob.h"
#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "bridge/engine/VideoNackReceiveJob.h"
#include "codec/Opus.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "memory/Map.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/Transport.h"
#include "transport/TransportImpl.h"
#include "utils/SimpleJson.h"
#include "utils/Span.h"
#include "webrtc/DataChannel.h"
#include <cstring>

using namespace bridge;

namespace
{

const int16_t mixSampleScaleFactor = 4;

memory::UniquePacket createGoodBye(uint32_t ssrc, memory::PacketPoolAllocator& allocator)
{
    auto packet = memory::makeUniquePacket(allocator);
    if (packet)
    {
        auto* rtcp = rtp::RtcpGoodbye::create(packet->get(), ssrc);
        packet->setLength(rtcp->header.size());
    }
    return packet;
}

class RemovePacketCacheJob : public jobmanager::CountedJob
{
public:
    RemovePacketCacheJob(bridge::EngineMixer& mixer,
        transport::Transport& transport,
        bridge::SsrcOutboundContext& outboundContext,
        bridge::MixerManagerAsync& mixerManager)
        : CountedJob(transport.getJobCounter()),
          _mixer(mixer),
          _outboundContext(outboundContext),
          _mixerManager(mixerManager),
          _endpointIdHash(transport.getEndpointIdHash())
    {
    }

    void run() override
    {
        if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
        {
            const auto posted = _mixerManager.asyncFreeVideoPacketCache(_mixer, _outboundContext.ssrc, _endpointIdHash);
            if (posted)
            {
                _outboundContext.packetCache.clear();
            }
        }
    }

private:
    bridge::EngineMixer& _mixer;
    bridge::SsrcOutboundContext& _outboundContext;
    bridge::MixerManagerAsync& _mixerManager;
    size_t _endpointIdHash;
};

class FinalizeNonSsrcRewriteOutboundContextJob : public jobmanager::CountedJob
{
public:
    FinalizeNonSsrcRewriteOutboundContextJob(bridge::EngineMixer& mixer,
        transport::RtcTransport& transport,
        bridge::SsrcOutboundContext& outboundContext,
        concurrency::SynchronizationContext& engineSyncContext,
        bridge::MixerManagerAsync& mixerManager,
        uint32_t feedbackSsrc)
        : CountedJob(transport.getJobCounter()),
          _mixer(mixer),
          _outboundContext(outboundContext),
          _transport(transport),
          _engineSyncContext(engineSyncContext),
          _mixerManager(mixerManager),
          _feedbackSsrc(feedbackSsrc)
    {
    }

    void run() override
    {
        const size_t endpointIdHash = _transport.getEndpointIdHash();
        const uint32_t ssrc = _outboundContext.ssrc;

        auto packet = createGoodBye(ssrc, _outboundContext.allocator);
        if (packet)
        {
            _transport.protectAndSend(std::move(packet));
        }

        if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
        {
            // Run RemovePacketCacheJob inside of this job
            RemovePacketCacheJob(_mixer, _transport, _outboundContext, _mixerManager).run();
        }

        _transport.removeSrtpLocalSsrc(ssrc);

        _engineSyncContext.post(utils::bind(&EngineMixer::onOutboundContextFinalized,
            &_mixer,
            endpointIdHash,
            ssrc,
            _feedbackSsrc,
            _outboundContext.rtpMap.isVideo()));
    }

private:
    bridge::EngineMixer& _mixer;
    bridge::SsrcOutboundContext& _outboundContext;
    transport::RtcTransport& _transport;
    concurrency::SynchronizationContext& _engineSyncContext;
    bridge::MixerManagerAsync& _mixerManager;
    uint32_t _feedbackSsrc;
};

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

const size_t EngineMixer::samplesPerIteration;
constexpr size_t EngineMixer::iterationDurationMs;

EngineMixer::EngineMixer(const std::string& id,
    jobmanager::JobManager& jobManager,
    const concurrency::SynchronizationContext& engineSyncContext,
    jobmanager::JobManager& backgroundJobQueue,
    MixerManagerAsync& messageListener,
    const uint32_t localVideoSsrc,
    const config::Config& config,
    memory::PacketPoolAllocator& sendAllocator,
    memory::AudioPacketPoolAllocator& audioAllocator,
    const std::vector<uint32_t>& audioSsrcs,
    const std::vector<api::SimulcastGroup>& videoSsrcs,
    const uint32_t lastN)
    : _id(id),
      _loggableId("EngineMixer"),
      _jobManager(jobManager),
      _engineSyncContext(engineSyncContext),
      _messageListener(messageListener),
      _mixerSsrcAudioBuffers(maxSsrcs),
      _incomingBarbellSctp(128),
      _incomingForwarderAudioRtp(maxPendingPackets),
      _incomingMixerAudioRtp(maxPendingPackets),
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
      _sendAllocator(sendAllocator),
      _audioAllocator(audioAllocator),
      _lastReceiveTime(utils::Time::getAbsoluteTime()),
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
      _lastRecordingAckProcessed(utils::Time::getAbsoluteTime()),
      _slidesPresent(false)
{
    assert(audioSsrcs.size() <= SsrcRewrite::ssrcArraySize);
    assert(videoSsrcs.size() <= SsrcRewrite::ssrcArraySize);

    memset(_mixedData, 0, samplesPerIteration * sizeof(int16_t));
}

EngineMixer::~EngineMixer() {}

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
        engineAudioStream->audioMixed ? 't' : 'f');

    _engineAudioStreams.emplace(endpointIdHash, engineAudioStream);
    if (engineAudioStream->audioMixed)
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

    if (engineAudioStream->audioMixed)
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

void EngineMixer::addAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer)
{
    _mixerSsrcAudioBuffers.erase(ssrc);
    _mixerSsrcAudioBuffers.emplace(ssrc, audioBuffer);
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
    _incomingMixerAudioRtp.clear();
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

    // 1. Process all incoming packets
    processBarbellSctp(engineIterationStartTimestamp);
    forwardPackets(engineIterationStartTimestamp);
    processIncomingRtcpPackets(engineIterationStartTimestamp);

    // 2. Check for stale streams
    checkPacketCounters(engineIterationStartTimestamp);

    runDominantSpeakerCheck(engineIterationStartTimestamp);
    sendMessagesToNewDataStreams();
    markSsrcsInUse(engineIterationStartTimestamp);
    processMissingPackets(engineIterationStartTimestamp); // must run after checkPacketCounters

    // 3. Update bandwidth estimates
    if (_config.rctl.useUplinkEstimate)
    {
        checkIfRateControlIsNeeded(engineIterationStartTimestamp);
        updateDirectorUplinkEstimates(engineIterationStartTimestamp);
        checkVideoBandwidth(engineIterationStartTimestamp);
    }

    // 4. Perform audio mixing
    mixSsrcBuffers();
    processAudioStreams();

    // 5. Check if Transports are alive
    removeIdleStreams(engineIterationStartTimestamp);

    // 6. Maintain transports.
    runTransportTicks(engineIterationStartTimestamp);
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

    {
        stats.audioInQueues = 0;
        stats.audioInQueueSamples = 0;
        stats.maxAudioInQueueSamples = 0;
        for (auto& audioBuffer : _mixerSsrcAudioBuffers)
        {
            if (!audioBuffer.second)
            {
                continue;
            }
            ++stats.audioInQueues;
            uint32_t len = audioBuffer.second->getLength() / 2;
            stats.audioInQueueSamples += len;
            stats.maxAudioInQueueSamples = std::max(stats.maxAudioInQueueSamples, len);
        }
    }

    return stats;
}

void EngineMixer::onAudioRtpPacketReceived(SsrcInboundContext& ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    // TODO we should also check if this ssrc is in use, mixed clients are present, or it needs to be forwarded over
    // barbell. If not, we can drop this packet.
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
            extendedSequenceNumber);
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
            logger::debug("Endpoint message %lu -> %lu %s",
                _loggableId.c_str(),
                fromEndpointIdHash,
                toEndpointIdHash,
                endpointMessage.get());
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
        logger::error("could not acquire inbound ssrc context ssrc %u", _loggableId.c_str(), ssrc);
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

void EngineMixer::onMixerAudioRtpPacketDecoded(SsrcInboundContext& inboundContext, memory::UniqueAudioPacket packet)
{
    assert(packet);
    if (!_incomingMixerAudioRtp.push(IncomingAudioPacketInfo(std::move(packet), &inboundContext)))
    {
        logger::error("Failed to push incoming mixer audio packet onto queue", getLoggableId().c_str());
        assert(false);
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
    const RtpMap& rtpMap)
{
    auto* ssrcOutboundContext = ssrcOutboundContexts.getItem(ssrc);
    if (ssrcOutboundContext)
    {
        return ssrcOutboundContext;
    }

    auto emplaceResult = ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, rtpMap);

    if (!emplaceResult.second && emplaceResult.first == ssrcOutboundContexts.end())
    {
        logger::error("Failed to create %s outbound context for ssrc %u, endpointIdHash %zu",
            _loggableId.c_str(),
            rtpMap.format == RtpMap::Format::OPUS ? "audio" : "video",
            ssrc,
            endpointIdHash);
        return nullptr;
    }

    logger::info("Created new %s outbound context for stream, endpointIdHash %zu, ssrc %u",
        _loggableId.c_str(),
        rtpMap.format == RtpMap::Format::OPUS ? "audio" : "video",
        endpointIdHash,
        ssrc);

    return &emplaceResult.first->second;
}

SsrcOutboundContext* EngineMixer::obtainOutboundForwardSsrcContext(size_t endpointIdHash,
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext>& ssrcOutboundContexts,
    const uint32_t ssrc,
    const RtpMap& rtpMap)
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

    auto emplaceResult = ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, rtpMap);

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
    if (audioStream && audioStream->rtpMap.payloadType == payloadType)
    {
        if (!audioStream->remoteSsrc.isSet() || audioStream->remoteSsrc.get() != ssrc)
        {
            return nullptr;
        }

        auto rtpMap = audioStream->rtpMap;
        if (!rtpMap.audioLevelExtId.isSet())
        {
            auto id = rtpMap.suggestAudioLevelExtensionId();
            if (id != RtpMap::ExtHeaderIdentifiers::EOL)
            {
                rtpMap.audioLevelExtId.set(id);
            }
        }

        auto emplaceResult = _allSsrcInboundContexts.emplace(ssrc, ssrc, rtpMap, sender, timestamp);

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
            _allSsrcInboundContexts.emplace(ssrc, ssrc, videoStream->feedbackRtpMap, sender, timestamp);
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
        if (!audioStream || &audioStream->transport == packetInfo.transport() || audioStream->audioMixed)
        {
            continue;
        }
        if (audioStream->ssrcRewrite && !sourceMapped)
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
        if (audioStream->ssrcRewrite)
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
                audioStream->transport);
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

void EngineMixer::processIncomingRtpPackets(const uint64_t timestamp)
{
    uint32_t numRtpPackets = 0;

    for (IncomingPacketInfo packetInfo; _incomingForwarderAudioRtp.pop(packetInfo);)
    {
        ++numRtpPackets;

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

    bool overrunLogSpamGuard = false;
    for (IncomingAudioPacketInfo packetInfo; _incomingMixerAudioRtp.pop(packetInfo);)
    {
        ++numRtpPackets;
        addPacketToMixerBuffers(packetInfo, timestamp, overrunLogSpamGuard);
    }

    if (numRtpPackets == 0)
    {
        const bool isIdle =
            utils::Time::diffGE(_lastReceiveTime, timestamp, _config.mixerInactivityTimeoutMs * utils::Time::ms);
        if (isIdle && !_hasSentTimeout)
        {
            _hasSentTimeout = _messageListener.asyncMixerTimedOut(*this);
        }
    }
    else
    {
        _lastReceiveTime = timestamp;
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
                videoStream->rtpMap);
        }
        else
        {
            // non rewrite recipients gets the video sent on same ssrc as level 0.
            ssrc = packetInfo.inboundContext()->defaultLevelSsrc;
            ssrcOutboundContext = obtainOutboundForwardSsrcContext(videoStream->endpointIdHash,
                videoStream->ssrcOutboundContexts,
                ssrc,
                videoStream->rtpMap);
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
                *this);
        }
        else
        {
            logger::warn("send allocator depleted FwdRewrite", _loggableId.c_str());
        }
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

        _lastReceiveTime = timestamp;
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

    auto* inboundContext = _ssrcInboundContexts.getItem(outboundContext->originalSsrc.load());
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
            rtcpSenderVideoStream->feedbackRtpMap);
    }
    else
    {
        rtxSsrcOutboundContext = obtainOutboundForwardSsrcContext(rtcpSenderVideoStream->endpointIdHash,
            rtcpSenderVideoStream->ssrcOutboundContexts,
            feedbackSsrc,
            rtcpSenderVideoStream->feedbackRtpMap);
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

inline void EngineMixer::processAudioStreams()
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

            if (!audioStream->audioMixed || !audioStream->transport.isConnected())
            {
                if (isContributingToMix)
                {
                    audioBuffer->drop(samplesPerIteration);
                }
                continue;
            }
        }

        if (!audioStream->audioMixed)
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
                if (peerAudioStream.remoteSsrc.isSet() && peerAudioStream.transport.isConnected() &&
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
    utils::StringBuilder<256> dominantSpeakerMessage;
    _activeMediaList->makeDominantSpeakerMessage(dominantSpeakerMessage);

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

void EngineMixer::markAssociatedAudioOutboundContextsForDeletion(const EngineAudioStream* senderAudioStream)
{
    if (!senderAudioStream->remoteSsrc.isSet())
    {
        return;
    }

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto* audioStream = audioStreamEntry.second;
        if (audioStream == senderAudioStream || audioStream->ssrcRewrite)
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
        const uint32_t slidesLimit = minUplinkEstimate * _config.slides.allocFactor;

        logger::info("limiting bitrate for ssrc %u, at %u",
            _loggableId.c_str(),
            presenterSimulcastLevel->ssrc,
            slidesLimit);

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
        engineVideoStream.feedbackRtpMap);

    if (outboundContext)
    {
        engineVideoStream.transport.postOnQueue(utils::bind(&transport::RtcTransport::setRtxProbeSource,
            &engineVideoStream.transport,
            engineVideoStream.localSsrc,
            &outboundContext->rewrite.lastSent.sequenceNumber,
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

bool EngineMixer::asyncAddAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer)
{
    return post(utils::bind(&EngineMixer::addAudioBuffer, this, ssrc, audioBuffer));
}

bool EngineMixer::asyncRemoveStream(const EngineAudioStream* engineAudioStream)
{
    return post([=]() { this->removeStream(engineAudioStream); });
}

bool EngineMixer::asyncRemoveStream(const EngineVideoStream* stream)
{
    return post([=]() { this->removeStream(stream); });
}

bool EngineMixer::asyncRemoveStream(const EngineDataStream* stream)
{
    return post([=]() { this->removeStream(stream); });
}

bool EngineMixer::asyncAddVideoPacketCache(const uint32_t ssrc,
    const size_t endpointIdHash,
    PacketCache* videoPacketCache)
{
    return post(utils::bind(&EngineMixer::addVideoPacketCache, this, ssrc, endpointIdHash, videoPacketCache));
}

bool EngineMixer::asyncReconfigureAudioStream(const transport::RtcTransport& transport, const uint32_t remoteSsrc)
{
    return post(utils::bind(&EngineMixer::reconfigureAudioStream, this, std::cref(transport), remoteSsrc));
}

bool EngineMixer::asyncStartTransport(transport::RtcTransport& transport)
{
    return post(utils::bind(&EngineMixer::startTransport, this, std::ref(transport)));
}

bool EngineMixer::asyncAddAudioStream(EngineAudioStream* engineAudioStream)
{
    return post(utils::bind(&EngineMixer::addAudioStream, this, engineAudioStream));
}

bool EngineMixer::asyncAddVideoStream(EngineVideoStream* engineVideoStream)
{
    return post(utils::bind(&EngineMixer::addVideoStream, this, engineVideoStream));
}

bool EngineMixer::asyncAddDataSteam(EngineDataStream* engineDataStream)
{
    return post(utils::bind(&EngineMixer::addDataSteam, this, engineDataStream));
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

bool EngineMixer::asyncHandleSctpControl(const size_t endpointIdHash, memory::UniquePacket& packet)
{
    return post(utils::bind(&EngineMixer::handleSctpControl, this, endpointIdHash, utils::moveParam(packet)));
}

} // namespace bridge
