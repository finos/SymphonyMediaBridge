#include "bridge/engine/EngineMixer.h"
#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/RecordingDescription.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/AudioForwarderRewriteAndSendJob.h"
#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMessage.h"
#include "bridge/engine/EngineMessageListener.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/ProcessMissingVideoPacketsJob.h"
#include "bridge/engine/ProcessUnackedRecordingEventPacketsJob.h"
#include "bridge/engine/RecordingAudioForwarderSendJob.h"
#include "bridge/engine/RecordingEventAckReceiveJob.h"
#include "bridge/engine/RecordingRtpNackReceiveJob.h"
#include "bridge/engine/RecordingSendEventJob.h"
#include "bridge/engine/SendEngineMessageJob.h"
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
#include "transport/recp/RecControlHeader.h"
#include "transport/recp/RecDominantSpeakerEventBuilder.h"
#include "transport/recp/RecStartStopEventBuilder.h"
#include "transport/recp/RecStreamAddedEventBuilder.h"
#include "transport/recp/RecStreamRemovedEventBuilder.h"
#include "utils/SimpleJson.h"
#include <cstring>

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

class RemoveInboundSsrcContextJob : public jobmanager::CountedJob
{
public:
    RemoveInboundSsrcContextJob(uint32_t ssrc, transport::Transport& transport, bridge::EngineMixer& engineMixer)
        : CountedJob(transport.getJobCounter()),
          _engineMixer(engineMixer),
          _ssrc(ssrc)
    {
    }

    void run() override { _engineMixer.tryRemoveInboundSsrc(_ssrc); }

private:
    bridge::EngineMixer& _engineMixer;
    uint32_t _ssrc;
};

class RemoveSrtpSsrcJob : public jobmanager::CountedJob
{
public:
    RemoveSrtpSsrcJob(transport::RtcTransport& transport, uint32_t ssrc)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _ssrc(ssrc)
    {
    }

    void run() override { _transport.removeSrtpLocalSsrc(_ssrc); }

private:
    transport::RtcTransport& _transport;
    uint32_t _ssrc;
};

class SetRtxProbeSourceJob : public jobmanager::CountedJob
{
public:
    SetRtxProbeSourceJob(transport::RtcTransport& transport,
        const uint32_t ssrc,
        uint32_t* sequenceCounter,
        const uint16_t payloadType)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _ssrc(ssrc),
          _sequenceCounter(sequenceCounter),
          _payloadType(payloadType)
    {
    }

    void run() override { _transport.setRtxProbeSource(_ssrc, _sequenceCounter, _payloadType); }

private:
    transport::RtcTransport& _transport;
    uint32_t _ssrc;
    uint32_t* _sequenceCounter;
    uint16_t _payloadType;
};

/**
 * @return true if this ssrc should be skipped and not forwarded to the videoStream.
 */
inline bool shouldSkipBecauseOfWhitelist(const bridge::EngineVideoStream& videoStream, const uint32_t ssrc)
{
    if (!videoStream._ssrcWhitelist.enabled)
    {
        return false;
    }

    switch (videoStream._ssrcWhitelist.numSsrcs)
    {
    case 0:
        return true;
    case 1:
        return videoStream._ssrcWhitelist.ssrcs[0] != ssrc;
    case 2:
        return videoStream._ssrcWhitelist.ssrcs[0] != ssrc && videoStream._ssrcWhitelist.ssrcs[1] != ssrc;
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
    EngineMessageListener& messageListener,
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
      _ssrcInboundContexts(maxSsrcs),
      _audioSsrcToUserIdMap(ActiveMediaList::maxParticipants),
      _localVideoSsrc(localVideoSsrc),
      _rtpTimestampSource(1000),
      _sendAllocator(sendAllocator),
      _audioAllocator(audioAllocator),
      _lastReceiveTime(utils::Time::getAbsoluteTime()),
      _noTicks(0),
      _ticksPerSSRCCheck(ticksPerSSRCCheck),
      _engineStreamDirector(std::make_unique<EngineStreamDirector>(config)),
      _activeMediaList(std::make_unique<ActiveMediaList>(_loggableId.getInstanceId(),
          audioSsrcs,
          videoSsrcs,
          lastN,
          config.audio.lastN,
          config.audio.activeTalkerSilenceThresholdDb)),
      _lastUplinkEstimateUpdate(0),
      _config(config),
      _lastN(lastN),
      _numMixedAudioStreams(0),
      _lastVideoBandwidthCheck(0),
      _lastVideoPacketProcessed(0),
      _probingVideoStreams(false),
      _minUplinkEstimate(0)
{
    assert(audioSsrcs.size() <= SsrcRewrite::ssrcArraySize);
    assert(videoSsrcs.size() <= SsrcRewrite::ssrcArraySize);

    memset(_mixedData, 0, samplesPerIteration * sizeof(int16_t));
}

EngineMixer::~EngineMixer() {}

void EngineMixer::addAudioStream(EngineAudioStream* engineAudioStream)
{
    const auto endpointIdHash = engineAudioStream->transport.getEndpointIdHash();
    if (_engineAudioStreams.find(endpointIdHash) != _engineAudioStreams.end())
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

void EngineMixer::removeAudioStream(EngineAudioStream* engineAudioStream)
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
        auto* context = obtainOutboundSsrcContext(engineAudioStream->endpointIdHash,
            engineAudioStream->ssrcOutboundContexts,
            engineAudioStream->localSsrc,
            engineAudioStream->rtpMap);
        if (context)
        {
            auto goodByePacket = createGoodBye(context->_ssrc, context->_allocator);
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
        markInboundContextForDeletion(engineAudioStream->remoteSsrc.get());
        _mixerSsrcAudioBuffers.erase(engineAudioStream->remoteSsrc.get());

        sendAudioStreamToRecording(*engineAudioStream, false);
    }

    _engineAudioStreams.erase(endpointIdHash);

    EngineMessage::Message message(EngineMessage::Type::AudioStreamRemoved);
    message.command.audioStreamRemoved.mixer = this;
    message.command.audioStreamRemoved.engineStream = engineAudioStream;
    engineAudioStream->transport.getJobQueue().addJob<SendEngineMessageJob>(engineAudioStream->transport,
        _messageListener,
        std::move(message));
}

void EngineMixer::addVideoStream(EngineVideoStream* engineVideoStream)
{
    const auto endpointIdHash = engineVideoStream->_endpointIdHash;
    if (_engineVideoStreams.find(endpointIdHash) != _engineVideoStreams.end())
    {
        return;
    }

    logger::debug("Add engineVideoStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineVideoStream->_transport.getLoggableId().c_str(),
        endpointIdHash);

    if (_probingVideoStreams)
    {
        startProbingVideoStream(*engineVideoStream);
    }
    const auto mapRevision = _activeMediaList->getMapRevision();
    _engineVideoStreams.emplace(endpointIdHash, engineVideoStream);
    if (engineVideoStream->_simulcastStream._numLevels > 0)
    {
        _engineStreamDirector->addParticipant(endpointIdHash, engineVideoStream->_simulcastStream);
        _activeMediaList->addVideoParticipant(endpointIdHash,
            engineVideoStream->_simulcastStream,
            engineVideoStream->_secondarySimulcastStream,
            engineVideoStream->_endpointId.c_str());
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

void EngineMixer::removeVideoStream(EngineVideoStream* engineVideoStream)
{
    engineVideoStream->_transport.getJobQueue().getJobManager().abortTimedJob(engineVideoStream->_transport.getId(),
        engineVideoStream->_localSsrc);

    auto* outboundContext = engineVideoStream->_ssrcOutboundContexts.getItem(engineVideoStream->_localSsrc);
    if (outboundContext)
    {
        outboundContext->_markedForDeletion = true;
        if (engineVideoStream->_transport.isConnected())
        {
            stopProbingVideoStream(*engineVideoStream);
            auto goodByePacket = createGoodBye(outboundContext->_ssrc, outboundContext->_allocator);
            if (goodByePacket)
            {
                engineVideoStream->_transport.getJobQueue().addJob<SendRtcpJob>(std::move(goodByePacket),
                    engineVideoStream->_transport);
            }
        }
    }

    if (engineVideoStream->_simulcastStream._numLevels != 0)
    {
        for (size_t i = 0; i < engineVideoStream->_simulcastStream._numLevels; ++i)
        {
            markInboundContextForDeletion(engineVideoStream->_simulcastStream._levels[i]._ssrc);
            markInboundContextForDeletion(engineVideoStream->_simulcastStream._levels[i]._feedbackSsrc);
        }

        markAssociatedVideoOutboundContextsForDeletion(engineVideoStream,
            engineVideoStream->_simulcastStream._levels[0]._ssrc,
            engineVideoStream->_simulcastStream._levels[0]._feedbackSsrc);
    }

    if (engineVideoStream->_secondarySimulcastStream.isSet() &&
        engineVideoStream->_secondarySimulcastStream.get()._numLevels != 0)
    {
        for (size_t i = 0; i < engineVideoStream->_simulcastStream._numLevels; ++i)
        {
            markInboundContextForDeletion(engineVideoStream->_secondarySimulcastStream.get()._levels[i]._ssrc);
            markInboundContextForDeletion(engineVideoStream->_secondarySimulcastStream.get()._levels[i]._feedbackSsrc);
        }

        markAssociatedVideoOutboundContextsForDeletion(engineVideoStream,
            engineVideoStream->_secondarySimulcastStream.get()._levels[0]._ssrc,
            engineVideoStream->_secondarySimulcastStream.get()._levels[0]._feedbackSsrc);
    }

    const auto endpointIdHash = engineVideoStream->_endpointIdHash;

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
        engineVideoStream->_transport.getLoggableId().c_str(),
        endpointIdHash);

    _engineVideoStreams.erase(endpointIdHash);

    EngineMessage::Message message(EngineMessage::Type::VideoStreamRemoved);
    message.command.videoStreamRemoved.mixer = this;
    message.command.videoStreamRemoved.engineStream = engineVideoStream;
    engineVideoStream->_transport.getJobQueue().addJob<SendEngineMessageJob>(engineVideoStream->_transport,
        _messageListener,
        std::move(message));
}

void EngineMixer::addRecordingStream(EngineRecordingStream* engineRecordingStream)
{
    const auto it = _engineRecordingStreams.find(engineRecordingStream->_endpointIdHash);
    if (it != _engineRecordingStreams.end())
    {
        assert(false);
        return;
    }

    _engineRecordingStreams.emplace(engineRecordingStream->_endpointIdHash, engineRecordingStream);
}

void EngineMixer::removeRecordingStream(EngineRecordingStream* engineRecordingStream)
{
    _engineStreamDirector->removeParticipant(engineRecordingStream->_endpointIdHash);
    _engineStreamDirector->removeParticipantPins(engineRecordingStream->_endpointIdHash);
    _engineRecordingStreams.erase(engineRecordingStream->_endpointIdHash);
}

void EngineMixer::updateRecordingStreamModalities(EngineRecordingStream* engineRecordingStream,
    bool isAudioEnabled,
    bool isVideoEnabled,
    bool isScreenSharingEnabled)
{
    if (!engineRecordingStream->_isReady)
    {
        // I think this is very unlikely or even impossible to happen
        logger::warn("Received a stream update modality but the stream is not ready yet. endpointIdHash %lu",
            _loggableId.c_str(),
            engineRecordingStream->_endpointIdHash);
        return;
    }

    if (engineRecordingStream->_isAudioEnabled != isAudioEnabled)
    {
        engineRecordingStream->_isAudioEnabled = isAudioEnabled;
        updateRecordingAudioStreams(*engineRecordingStream, isAudioEnabled);
    }

    if (engineRecordingStream->_isVideoEnabled != isVideoEnabled)
    {
        engineRecordingStream->_isVideoEnabled = isVideoEnabled;
        updateRecordingVideoStreams(*engineRecordingStream, SimulcastStream::VideoContentType::VIDEO, isVideoEnabled);
    }

    if (engineRecordingStream->_isScreenSharingEnabled != isScreenSharingEnabled)
    {
        engineRecordingStream->_isScreenSharingEnabled = isScreenSharingEnabled;
        updateRecordingVideoStreams(*engineRecordingStream,
            SimulcastStream::VideoContentType::SLIDES,
            isScreenSharingEnabled);
    }
}

void EngineMixer::addDataSteam(EngineDataStream* engineDataStream)
{
    const auto endpointIdHash = engineDataStream->_endpointIdHash;
    if (_engineDataStreams.find(endpointIdHash) != _engineDataStreams.end())
    {
        return;
    }

    logger::debug("Add engineDataStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineDataStream->_transport.getLoggableId().c_str(),
        endpointIdHash);

    _engineDataStreams.emplace(endpointIdHash, engineDataStream);
}

void EngineMixer::removeDataStream(EngineDataStream* engineDataStream)
{
    const auto endpointIdHash = engineDataStream->_endpointIdHash;
    logger::debug("Remove engineDataStream, transport %s, endpointIdHash %lu",
        _loggableId.c_str(),
        engineDataStream->_transport.getLoggableId().c_str(),
        endpointIdHash);

    _engineDataStreams.erase(endpointIdHash);
}

void EngineMixer::startTransport(transport::RtcTransport* transport)
{
    assert(transport);

    logger::debug("Starting transport %s", transport->getLoggableId().c_str(), _loggableId.c_str());
    transport->setDataReceiver(this);

    // start on transport allows incoming packets.
    // Postponing this until callbacks has been set and remote ice and dtls has been configured is vital to avoid
    // race condition and sync problems with ice session and srtpclient
    transport->start();
    transport->connect();
}

void EngineMixer::startRecordingTransport(transport::RecordingTransport* transport)
{
    assert(transport);

    logger::debug("Starting recording transport %s", transport->getLoggableId().c_str(), _loggableId.c_str());
    transport->setDataReceiver(this);

    transport->start();
    transport->connect();
}

void EngineMixer::reconfigureAudioStream(const transport::RtcTransport* transport, const uint32_t remoteSsrc)
{
    auto audioStreamItr = _engineAudioStreams.find(transport->getEndpointIdHash());
    if (audioStreamItr == _engineAudioStreams.end())
    {
        return;
    }
    auto engineAudioStream = audioStreamItr->second;

    if (engineAudioStream->remoteSsrc.isSet() && engineAudioStream->remoteSsrc.get() != remoteSsrc)
    {
        markInboundContextForDeletion(engineAudioStream->remoteSsrc.get());
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

void EngineMixer::reconfigureVideoStream(const transport::RtcTransport* transport,
    const SsrcWhitelist& ssrcWhitelist,
    const SimulcastStream& simulcastStream,
    const SimulcastStream* secondarySimulcastStream)
{
    const auto endpointIdHash = transport->getEndpointIdHash();

    auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr == _engineVideoStreams.end())
    {
        return;
    }
    auto engineVideoStream = videoStreamItr->second;

    const auto mapRevision = _activeMediaList->getMapRevision();
    if (engineVideoStream->_simulcastStream._numLevels != 0 &&
        (simulcastStream._numLevels == 0 ||
            simulcastStream._levels[0]._ssrc != engineVideoStream->_simulcastStream._levels[0]._ssrc))
    {
        removeVideoSsrcFromRecording(*engineVideoStream, engineVideoStream->_simulcastStream._levels[0]._ssrc);
    }

    if ((engineVideoStream->_secondarySimulcastStream.isSet() &&
            engineVideoStream->_secondarySimulcastStream.get()._numLevels != 0) &&
        (!secondarySimulcastStream ||
            secondarySimulcastStream->_levels[0]._ssrc !=
                engineVideoStream->_secondarySimulcastStream.get()._levels[0]._ssrc))
    {
        removeVideoSsrcFromRecording(*engineVideoStream,
            engineVideoStream->_secondarySimulcastStream.get()._levels[0]._ssrc);
    }

    engineVideoStream->_simulcastStream = simulcastStream;
    engineVideoStream->_secondarySimulcastStream = secondarySimulcastStream == nullptr
        ? utils::Optional<SimulcastStream>()
        : utils::Optional<SimulcastStream>(*secondarySimulcastStream);

    _engineStreamDirector->removeParticipant(endpointIdHash);
    _activeMediaList->removeVideoParticipant(endpointIdHash);

    if (engineVideoStream->_simulcastStream._numLevels > 0)
    {
        if (secondarySimulcastStream && secondarySimulcastStream->_numLevels > 0)
        {
            engineVideoStream->_secondarySimulcastStream.set(*secondarySimulcastStream);
            _engineStreamDirector->addParticipant(endpointIdHash,
                engineVideoStream->_simulcastStream,
                &engineVideoStream->_secondarySimulcastStream.get());
        }
        else
        {
            _engineStreamDirector->addParticipant(endpointIdHash, engineVideoStream->_simulcastStream);
        }

        _activeMediaList->addVideoParticipant(endpointIdHash,
            engineVideoStream->_simulcastStream,
            engineVideoStream->_secondarySimulcastStream,
            engineVideoStream->_endpointId.c_str());

        updateSimulcastLevelActiveState(*engineVideoStream, engineVideoStream->_simulcastStream);
        if (secondarySimulcastStream && secondarySimulcastStream->_numLevels > 0)
        {
            updateSimulcastLevelActiveState(*engineVideoStream, *secondarySimulcastStream);
        }
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

    memcpy(&engineVideoStream->_ssrcWhitelist, &ssrcWhitelist, sizeof(SsrcWhitelist));

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        auto otherVideoStream = videoStreamEntry.second;
        if (otherVideoStream == engineVideoStream)
        {
            continue;
        }
        sendPliForUsedSsrcs(*otherVideoStream);
    }
}

void EngineMixer::addVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* videoPacketCache)
{
    bridge::SsrcOutboundContext* ssrcOutboundContext = nullptr;

    auto* videoStream = _engineVideoStreams.getItem(endpointIdHash);
    if (videoStream)
    {
        ssrcOutboundContext = videoStream->_ssrcOutboundContexts.getItem(ssrc);
    }
    else
    {
        auto* barbell = _engineBarbells.getItem(endpointIdHash);
        if (barbell)
        {
            ssrcOutboundContext = barbell->ssrcOutboundContexts.getItem(ssrc);
        }
    }

    if (!ssrcOutboundContext || (ssrcOutboundContext->_packetCache.isSet() && ssrcOutboundContext->_packetCache.get()))
    {
        return;
    }

    ssrcOutboundContext->_packetCache.set(videoPacketCache);
}

void EngineMixer::addAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer)
{
    _mixerSsrcAudioBuffers.erase(ssrc);
    _mixerSsrcAudioBuffers.emplace(ssrc, audioBuffer);
}

void EngineMixer::recordingStart(EngineRecordingStream* stream, const RecordingDescription* desc)
{
    auto seq = stream->_recordingEventsOutboundContext._sequenceNumber++;
    auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : stream->_transports)
    {
        auto unackedPacketsTrackerItr = stream->_recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == stream->_recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send start recording event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        auto packet = recp::RecStartStopEventBuilder(_sendAllocator)
                          .setAudioEnabled(desc->_isAudioEnabled)
                          .setVideoEnabled(desc->_isVideoEnabled)
                          .setSequenceNumber(seq)
                          .setTimestamp(timestamp)
                          .setRecordingId(desc->_recordingId)
                          .setUserId(desc->_ownerId)
                          .build();
        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail. We have to find a way to report this glitch
            logger::error("No space available to allocate rec start event", _loggableId.c_str());
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            stream->_recordingEventsOutboundContext._packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::recordingStop(EngineRecordingStream* stream, const RecordingDescription* desc)
{
    const auto sequenceNumber = stream->_recordingEventsOutboundContext._sequenceNumber++;
    const auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : stream->_transports)
    {
        auto unackedPacketsTrackerItr = stream->_recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == stream->_recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send stop recording event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        auto packet = recp::RecStartStopEventBuilder(_sendAllocator)
                          .setAudioEnabled(false)
                          .setVideoEnabled(false)
                          .setSequenceNumber(sequenceNumber)
                          .setTimestamp(timestamp)
                          .setRecordingId(desc->_recordingId)
                          .setUserId(desc->_ownerId)
                          .build();

        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as we will not know when it finish. We have to find a way to report this glitch
            logger::error("No space available to allocate rec stop event", _loggableId.c_str());
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            stream->_recordingEventsOutboundContext._packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::addRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* packetCache)
{
    assert(endpointIdHash);

    auto recordingStreamItr = _engineRecordingStreams.find(endpointIdHash);
    if (recordingStreamItr == _engineRecordingStreams.end())
    {
        return;
    }

    auto outboundContext = recordingStreamItr->second->_ssrcOutboundContexts.getItem(ssrc);
    if (!outboundContext || (outboundContext->_packetCache.isSet() && outboundContext->_packetCache.get()))
    {
        return;
    }

    outboundContext->_packetCache.set(packetCache);
}

void EngineMixer::addTransportToRecordingStream(const size_t streamIdHash,
    transport::RecordingTransport* transport,
    UnackedPacketsTracker* recUnackedPacketsTracker)
{
    auto recordingStreamItr = _engineRecordingStreams.find(streamIdHash);
    if (recordingStreamItr == _engineRecordingStreams.end())
    {
        return;
    }

    auto recordingStream = recordingStreamItr->second;
    recordingStream->_transports.emplace(transport->getEndpointIdHash(), *transport);
    recordingStream->_recEventUnackedPacketsTracker.emplace(transport->getEndpointIdHash(), *recUnackedPacketsTracker);

    if (!recordingStream->_isReady)
    {
        recordingStream->_isReady = true;
        startRecordingAllCurrentStreams(*recordingStream);
    }
}

void EngineMixer::removeTransportFromRecordingStream(const size_t streamIdHash, const size_t endpointIdHash)
{
    auto recordingStreamItr = _engineRecordingStreams.find(streamIdHash);
    if (recordingStreamItr == _engineRecordingStreams.end())
    {
        return;
    }

    auto recordingStream = recordingStreamItr->second;
    recordingStream->_transports.erase(endpointIdHash);
    recordingStream->_recEventUnackedPacketsTracker.erase(endpointIdHash);

    EngineMessage::Message message(EngineMessage::Type::RemoveRecordingTransport);
    message.command.removeRecordingTransport.streamId = recordingStream->_id.c_str();
    message.command.removeRecordingTransport.endpointIdHash = endpointIdHash;
    _messageListener.onMessage(std::move(message));
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
}

void EngineMixer::processMissingPackets(const uint64_t timestamp)
{
    for (auto& ssrcInboundContextEntry : _ssrcInboundContexts)
    {
        auto& ssrcInboundContext = ssrcInboundContextEntry.second;
        if (ssrcInboundContext._rtpMap._format != RtpMap::Format::VP8 || ssrcInboundContext._markedForDeletion)
        {
            continue;
        }
        auto videoMissingPacketsTracker = ssrcInboundContext._videoMissingPacketsTracker.get();
        if (!videoMissingPacketsTracker || !videoMissingPacketsTracker->shouldProcess(timestamp / 1000000ULL))
        {
            continue;
        }

        auto videoStreamItr = _engineVideoStreams.find(ssrcInboundContext._sender->getEndpointIdHash());
        if (videoStreamItr == _engineVideoStreams.end())
        {
            continue;
        }
        auto videoStream = videoStreamItr->second;

        videoStream->_transport.getJobQueue().addJob<bridge::ProcessMissingVideoPacketsJob>(ssrcInboundContext,
            videoStream->_localSsrc,
            videoStream->_transport,
            _sendAllocator);
    }

    processRecordingMissingPackets(timestamp);
}

void EngineMixer::runDominantSpeakerCheck(const uint64_t engineIterationStartTimestamp)
{
    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;
    bool audioMapChanged = false;
    _activeMediaList->process(engineIterationStartTimestamp, dominantSpeakerChanged, videoMapChanged, audioMapChanged);

    if (dominantSpeakerChanged)
    {
        const auto dominantSpeaker = _activeMediaList->getDominantSpeaker();
        sendDominantSpeakerMessageToAll(dominantSpeaker);
        sendLastNListMessageToAll();
    }

    if (videoMapChanged)
    {
        sendUserMediaMapMessageToAll();
    }
    if (audioMapChanged || videoMapChanged)
    {
        sendUserMediaMapMessageOverBarbells();
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
        if (!videoStreamEntry.second->_transport.isConnected())
        {
            continue;
        }

        auto videoStream = videoStreamEntry.second;
        const auto uplinkEstimateKbps = videoStream->_transport.getUplinkEstimateKbps();
        if (uplinkEstimateKbps == 0 ||
            !_engineStreamDirector->setUplinkEstimateKbps(videoStream->_endpointIdHash,
                uplinkEstimateKbps,
                engineIterationStartTimestamp))
        {
            continue;
        }

        const auto pinTarget = _engineStreamDirector->getPinTarget(videoStream->_endpointIdHash);
        if (!pinTarget)
        {
            continue;
        }

        auto pinnedVideoStreamItr = _engineVideoStreams.find(pinTarget);
        if (pinnedVideoStreamItr == _engineVideoStreams.end())
        {
            continue;
        }
        sendPliForUsedSsrcs(*pinnedVideoStreamItr->second);
    }
}

void EngineMixer::tryRemoveInboundSsrc(uint32_t ssrc)
{
    auto contextIt = _ssrcInboundContexts.find(ssrc);
    if (contextIt != _ssrcInboundContexts.end() && contextIt->second._markedForDeletion)
    {
        if (contextIt->second._rtxSsrc.isSet())
        {
            auto buddySsrcIt = _ssrcInboundContexts.find(contextIt->second._rtxSsrc.get());
            if (buddySsrcIt != _ssrcInboundContexts.end())
            {
                logger::info("Removing idle inbound context feedback ssrc %u, main ssrc %u",
                    _loggableId.c_str(),
                    buddySsrcIt->first,
                    contextIt->first);

                EngineMessage::Message message(EngineMessage::Type::InboundSsrcRemoved);
                message.command.ssrcInboundRemoved = {this,
                    buddySsrcIt->first,
                    buddySsrcIt->second._opusDecoder.release()};

                _ssrcInboundContexts.erase(buddySsrcIt->first);
                _messageListener.onMessage(std::move(message));
            }
        }

        logger::info("Removing idle inbound context ssrc %u", _loggableId.c_str(), contextIt->first);

        EngineMessage::Message message(EngineMessage::Type::InboundSsrcRemoved);
        message.command.ssrcInboundRemoved.mixer = this;
        message.command.ssrcInboundRemoved.ssrc = ssrc;
        message.command.ssrcInboundRemoved.opusDecoder = contextIt->second._opusDecoder.release();
        _ssrcInboundContexts.erase(ssrc);
        _messageListener.onMessage(std::move(message));
    }
}

void EngineMixer::checkPacketCounters(const uint64_t timestamp)
{
    if ((_noTicks++) <= _ticksPerSSRCCheck)
    {
        return;
    }

    for (auto& inboundContextEntry : _ssrcInboundContexts)
    {
        auto& inboundContext = inboundContextEntry.second;
        const auto ssrc = inboundContextEntry.first;
        const auto endpointIdHash = inboundContext._sender->getEndpointIdHash();
        auto receiveCounters = inboundContext._sender->getCumulativeReceiveCounters(ssrc);

        if (utils::Time::diffGT(inboundContext._lastReceiveTime.load(), timestamp, utils::Time::sec * 1) &&
            receiveCounters.packets > 5 && inboundContext._activeMedia &&
            inboundContext._rtpMap._format != RtpMap::Format::VP8RTX)
        {
            auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
            if (videoStreamItr != _engineVideoStreams.end())
            {
                if (_engineStreamDirector->streamActiveStateChanged(endpointIdHash, ssrc, false))
                {
                    sendPliForUsedSsrcs(*videoStreamItr->second);
                }
                inboundContext._inactiveCount++;

                // The reason for checking if the ssrc is equal to inboundContext._rewriteSsrc, is because that is the
                // default simulcast level. We don't want to drop the default level even if it's unstable.
                if (inboundContext._inactiveCount >= _config.dropInboundAfterInactive.get() &&
                    ssrc != inboundContext._rewriteSsrc)
                {
                    logger::info("Inbound packets ssrc %u, transport %s, endpointIdHash %lu will be dropped",
                        _loggableId.c_str(),
                        inboundContext._ssrc,
                        inboundContext._sender->getLoggableId().c_str(),
                        endpointIdHash);

                    inboundContext._shouldDropPackets = true;
                }
            }

            inboundContext._activeMedia = false;
        }

        if (utils::Time::diffGT(inboundContext._lastReceiveTime.load(), timestamp, utils::Time::minute * 5) &&
            inboundContext._rtpMap._format != RtpMap::Format::VP8RTX)
        {
            if (!inboundContext._markedForDeletion && !inboundContext._idle)
            {
                logger::info("Inbound context ssrc %u has been idle for 5 minutes", _loggableId.c_str(), ssrc);
                inboundContext._idle = true;
                continue;
            }
            else if (!inboundContext._markedForDeletion && inboundContext._idle)
            {
                continue;
            }

            // if previous remove job is still pending, we may add another nop job
            inboundContext._sender->getJobQueue().addJob<RemoveInboundSsrcContextJob>(ssrc,
                *inboundContext._sender,
                *this);
        }
    }

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        const auto endpointIdHash = videoStreamEntry.first;
        for (auto& outboundContextEntry : videoStreamEntry.second->_ssrcOutboundContexts)
        {
            auto& outboundContext = outboundContextEntry.second;

            if (utils::Time::diffGT(outboundContext._lastSendTime, timestamp, utils::Time::sec * 30) &&
                outboundContext._rtpMap._format != RtpMap::Format::VP8RTX)
            {
                if (!outboundContext._markedForDeletion && !outboundContext._idle)
                {
                    logger::info("Outbound context ssrc %u, endpointIdHash %lu has been idle for 30 seconds",
                        _loggableId.c_str(),
                        outboundContextEntry.first,
                        endpointIdHash);

                    outboundContext._idle = true;
                    continue;
                }
                else if (!outboundContext._markedForDeletion && outboundContext._idle)
                {
                    continue;
                }

                uint32_t feedbackSsrc;
                if (_engineStreamDirector->getFeedbackSsrc(outboundContext._ssrc, feedbackSsrc))
                {
                    logger::info("Removing idle outbound context feedback ssrc %u, main ssrc %u, endpointIdHash %lu",
                        _loggableId.c_str(),
                        feedbackSsrc,
                        outboundContextEntry.first,
                        endpointIdHash);
                    videoStreamEntry.second->_transport.getJobQueue().addJob<RemoveSrtpSsrcJob>(
                        videoStreamEntry.second->_transport,
                        feedbackSsrc);
                    videoStreamEntry.second->_ssrcOutboundContexts.erase(feedbackSsrc);
                }

                // Pending jobs with reference to this ssrc context has had 30s to complete.
                // Removing the video packet cache can crash unfinished jobs.
                {
                    EngineMessage::Message message(EngineMessage::Type::FreeVideoPacketCache);
                    message.command.freeVideoPacketCache.mixer = this;
                    message.command.freeVideoPacketCache.ssrc = outboundContextEntry.first;
                    message.command.freeVideoPacketCache.endpointIdHash = videoStreamEntry.first;
                    _messageListener.onMessage(std::move(message));
                }

                logger::info("Removing idle outbound context ssrc %u, endpointIdHash %lu",
                    _loggableId.c_str(),
                    outboundContextEntry.first,
                    endpointIdHash);
                videoStreamEntry.second->_transport.getJobQueue().addJob<RemoveSrtpSsrcJob>(
                    videoStreamEntry.second->_transport,
                    outboundContextEntry.first);
                videoStreamEntry.second->_ssrcOutboundContexts.erase(outboundContextEntry.first);
            }
        }
    }

    for (auto& recordingStreamEntry : _engineRecordingStreams)
    {
        const auto endpointIdHash = recordingStreamEntry.first;
        for (auto& outboundContextEntry : recordingStreamEntry.second->_ssrcOutboundContexts)
        {
            auto& outboundContext = outboundContextEntry.second;
            if (utils::Time::diffGT(outboundContext._lastSendTime, timestamp, utils::Time::sec * 30))
            {
                if (!outboundContext._markedForDeletion && !outboundContext._idle)
                {
                    logger::info("Outbound context ssrc %u, rec endpointIdHash %lu has been idle for 30 seconds",
                        _loggableId.c_str(),
                        outboundContextEntry.first,
                        endpointIdHash);

                    outboundContext._idle = true;
                    continue;
                }
                else if (!outboundContext._markedForDeletion && outboundContext._idle)
                {
                    continue;
                }

                EngineMessage::Message message(EngineMessage::Type::FreeRecordingRtpPacketCache);
                message.command.freeRecordingRtpPacketCache.mixer = this;
                message.command.freeRecordingRtpPacketCache.ssrc = outboundContext._ssrc;
                message.command.freeRecordingRtpPacketCache.endpointIdHash = recordingStreamEntry.first;
                _messageListener.onMessage(std::move(message));

                logger::info("Removing idle outbound context ssrc %u, rec endpointIdHash %lu",
                    _loggableId.c_str(),
                    outboundContextEntry.first,
                    endpointIdHash);
                recordingStreamEntry.second->_ssrcOutboundContexts.erase(outboundContextEntry.first);
            }
        }
    }

    _noTicks = 0;
}

EngineStats::MixerStats EngineMixer::gatherStats(const uint64_t iterationStartTime)
{
    EngineStats::MixerStats stats;
    uint64_t idleTimestamp = iterationStartTime - utils::Time::sec * 2;

    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        const auto audioRecvCounters = audioStreamEntry.second->transport.getAudioReceiveCounters(idleTimestamp);
        const auto audioSendCounters = audioStreamEntry.second->transport.getAudioSendCounters(idleTimestamp);
        const auto videoRecvCounters = audioStreamEntry.second->transport.getVideoReceiveCounters(idleTimestamp);
        const auto videoSendCounters = audioStreamEntry.second->transport.getVideoSendCounters(idleTimestamp);
        const auto pacingQueueCount = audioStreamEntry.second->transport.getPacingQueueCount();
        const auto rtxPacingQueueCount = audioStreamEntry.second->transport.getRtxPacingQueueCount();

        stats.inbound.audio += audioRecvCounters;
        stats.outbound.audio += audioSendCounters;
        stats.inbound.video += videoRecvCounters;
        stats.outbound.video += videoSendCounters;
        stats.inbound.transport.addBandwidthGroup(audioStreamEntry.second->transport.getDownlinkEstimateKbps());
        stats.inbound.transport.addRttGroup(audioStreamEntry.second->transport.getRtt() / utils::Time::ms);
        stats.inbound.transport.addLossGroup((audioRecvCounters + videoRecvCounters).getReceiveLossRatio());
        stats.outbound.transport.addLossGroup((audioSendCounters + videoSendCounters).getSendLossRatio());
        stats.pacingQueue += pacingQueueCount;
        stats.rtxPacingQueue += rtxPacingQueueCount;
    }

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        if (!_engineAudioStreams.contains(videoStreamEntry.first))
        {
            const auto videoRecvCounters = videoStreamEntry.second->_transport.getVideoReceiveCounters(idleTimestamp);
            const auto videoSendCounters = videoStreamEntry.second->_transport.getVideoSendCounters(idleTimestamp);
            const auto pacingQueueCount = videoStreamEntry.second->_transport.getPacingQueueCount();
            const auto rtxPacingQueueCount = videoStreamEntry.second->_transport.getRtxPacingQueueCount();

            stats.inbound.video += videoRecvCounters;
            stats.outbound.video += videoSendCounters;
            stats.inbound.transport.addBandwidthGroup(videoStreamEntry.second->_transport.getDownlinkEstimateKbps());
            stats.inbound.transport.addRttGroup(videoStreamEntry.second->_transport.getRtt() / utils::Time::ms);
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

void EngineMixer::onVideoRtpPacketReceived(SsrcInboundContext* ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    const bool isFromBarbell = EngineBarbell::isFromBarbell(sender->getTag());
    const auto endpointIdHash = packet->endpointIdHash;

    EngineVideoStream* videoStream = nullptr;
    auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr != _engineVideoStreams.end())
    {
        videoStream = videoStreamItr->second;
    }
    if (!videoStream && !isFromBarbell)
    {
        return;
    }

    if (ssrcContext->_shouldDropPackets)
    {
        return;
    }

    if (!ssrcContext->_activeMedia)
    {
        if (_engineStreamDirector->streamActiveStateChanged(endpointIdHash, ssrcContext->_ssrc, true))
        {
            if (videoStream)
            {
                sendPliForUsedSsrcs(*videoStream);
            }
            // TODO must send PLI for barbelled streams also
        }
    }

    // This must happen after checking ssrcContext->_activeMedia above, so that we can detect streamActiveStateChanged
    ssrcContext->onRtpPacket(timestamp);

    const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(endpointIdHash);
    const bool mustBeForwardedOnBarbells = isSenderInLastNList && !_engineBarbells.empty() && !isFromBarbell;

    if (!mustBeForwardedOnBarbells &&
        !_engineStreamDirector->isSsrcUsed(ssrcContext->_ssrc,
            endpointIdHash,
            isSenderInLastNList,
            _engineRecordingStreams.size()))
    {
        return;
    }

    sender->getJobQueue().addJob<bridge::VideoForwarderReceiveJob>(std::move(packet),
        _sendAllocator,
        sender,
        *this,
        *ssrcContext,
        _localVideoSsrc,
        extendedSequenceNumber,
        timestamp);
}

utils::Optional<uint32_t> EngineMixer::findMainSsrc(size_t barbellIdHash, uint32_t feedbackSsrc)
{
    auto it = _engineBarbells.find(barbellIdHash);
    if (it == _engineBarbells.end())
    {
        return utils::Optional<uint32_t>();
    }

    auto& barbell = *it->second;
    auto videoStreamIt = barbell.videoSsrcMap.find(feedbackSsrc);
    if (videoStreamIt == barbell.videoSsrcMap.end())
    {
        assert(false);
        return utils::Optional<uint32_t>();
    }
    auto* videoStream = videoStreamIt->second;
    return videoStream->stream.getMainSsrcFor(feedbackSsrc);
}

void EngineMixer::onVideoRtpRtxPacketReceived(SsrcInboundContext* ssrcContext,
    transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
{
    const bool isFromBarbell = EngineBarbell::isFromBarbell(sender->getTag());
    const auto endpointIdHash = packet->endpointIdHash;
    EngineVideoStream* videoStream = nullptr;
    auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr != _engineVideoStreams.end())
    {
        videoStream = videoStreamItr->second;
    }
    if (!videoStream && !isFromBarbell)
    {
        return;
    }

    uint32_t mainSsrc;
    const uint32_t feedbackSsrc = ssrcContext->_ssrc;
    if (videoStream && !_engineStreamDirector->getSsrc(videoStream->_endpointIdHash, feedbackSsrc, mainSsrc))
    {
        return;
    }
    else if (isFromBarbell)
    {
        auto ssrc = findMainSsrc(sender->getEndpointIdHash(), feedbackSsrc);
        if (!ssrc.isSet())
        {
            return;
        }
        mainSsrc = ssrc.get();
    }

    auto mainSsrcContextItr = _ssrcInboundContexts.find(mainSsrc);
    if (mainSsrcContextItr == _ssrcInboundContexts.end())
    {
        return;
    }
    auto& mainSsrcContext = mainSsrcContextItr->second;
    if (mainSsrcContext._shouldDropPackets)
    {
        return;
    }
    mainSsrcContext._lastReceiveTime = timestamp;

    if (!ssrcContext->_videoMissingPacketsTracker.get())
    {
        if (!mainSsrcContext._videoMissingPacketsTracker.get())
        {
            return;
        }
        ssrcContext->_videoMissingPacketsTracker = mainSsrcContext._videoMissingPacketsTracker;
    }

    const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(endpointIdHash);
    const bool mustBeForwardedOnBarbells = isSenderInLastNList && !_engineBarbells.empty() && !isFromBarbell;

    if (!mustBeForwardedOnBarbells &&
        !_engineStreamDirector->isSsrcUsed(mainSsrc,
            videoStream->_endpointIdHash,
            isSenderInLastNList,
            _engineRecordingStreams.size()))
    {
        return;
    }

    sender->getJobQueue().addJob<bridge::VideoForwarderRtxReceiveJob>(std::move(packet),
        sender,
        *this,
        *ssrcContext,
        mainSsrc,
        extendedSequenceNumber);
}

void EngineMixer::onConnected(transport::RtcTransport* sender)
{
    logger::debug("transport connected", sender->getLoggableId().c_str());

    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        auto videoStream = videoStreamEntry.second;
        if (&videoStream->_transport == sender)
        {
            continue;
        }

        sendPliForUsedSsrcs(*videoStream);
    }

    auto barbell = _engineBarbells.getItem(sender->getEndpointIdHash());
    if (barbell && barbell->transport.isDtlsClient())
    {
        barbell->transport.connectSctp();
    }
}

void EngineMixer::handleSctpControl(const size_t endpointIdHash, const memory::Packet& packet)
{
    auto& header = webrtc::streamMessageHeader(packet);

    auto dataStreamItr = _engineDataStreams.find(endpointIdHash);
    if (dataStreamItr != _engineDataStreams.cend())
    {
        auto engineStream = dataStreamItr->second;

        engineStream->_stream.onSctpMessage(&engineStream->_transport,
            header.id,
            header.sequenceNumber,
            header.payloadProtocol,
            header.data(),
            packet.getLength() - sizeof(header));
    }
    else
    {
        auto barbellIt = _engineBarbells.find(endpointIdHash);
        if (barbellIt != _engineBarbells.cend())
        {
            barbellIt->second->dataChannel.onSctpMessage(&barbellIt->second->transport,
                header.id,
                header.sequenceNumber,
                header.payloadProtocol,
                header.data(),
                packet.getLength() - sizeof(header));
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

    if (oldTarget)
    {
        auto videoStreamItr = _engineVideoStreams.find(oldTarget);
        if (videoStreamItr != _engineVideoStreams.end())
        {
            sendPliForUsedSsrcs(*videoStreamItr->second);
        }
    }

    if (targetEndpointIdHash)
    {
        auto videoStreamItr = _engineVideoStreams.find(targetEndpointIdHash);
        if (videoStreamItr != _engineVideoStreams.end())
        {
            sendPliForUsedSsrcs(*videoStreamItr->second);
        }
    }

    auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr != _engineVideoStreams.end())
    {
        const auto videoStream = videoStreamItr->second;
        const auto oldPinSsrc = videoStream->_pinSsrc;
        if (oldPinSsrc.isSet())
        {
            videoStream->_videoPinSsrcs.push(oldPinSsrc.get());
            videoStream->_pinSsrc.clear();
        }
        if (targetEndpointIdHash)
        {
            SimulcastLevel newPinSsrc;
            if (videoStream->_videoPinSsrcs.pop(newPinSsrc))
            {
                videoStream->_pinSsrc.set(newPinSsrc);
                logger::debug("EndpointIdHash %zu pin %zu, pin ssrc %u",
                    _loggableId.c_str(),
                    endpointIdHash,
                    targetEndpointIdHash,
                    newPinSsrc._ssrc);
            }
        }
    }

    sendLastNListMessage(endpointIdHash);
    sendUserMediaMapMessage(endpointIdHash);
}

void EngineMixer::sendEndpointMessage(const size_t toEndpointIdHash,
    const size_t fromEndpointIdHash,
    const char* message)
{
    if (!fromEndpointIdHash)
    {
        assert(false);
        return;
    }

    auto fromDataStreamItr = _engineDataStreams.find(fromEndpointIdHash);
    if (fromDataStreamItr == _engineDataStreams.end())
    {
        return;
    }

    utils::StringBuilder<2048> endpointMessage;

    if (toEndpointIdHash)
    {
        auto toDataStreamItr = _engineDataStreams.find(toEndpointIdHash);
        if (toDataStreamItr == _engineDataStreams.end() || !toDataStreamItr->second->_stream.isOpen())
        {
            return;
        }

        api::DataChannelMessage::makeEndpointMessage(endpointMessage,
            toDataStreamItr->second->_endpointId,
            fromDataStreamItr->second->_endpointId,
            message);

        toDataStreamItr->second->_stream.sendString(endpointMessage.get(), endpointMessage.getLength());
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
            if (dataStreamEntry.first == fromEndpointIdHash || !dataStreamEntry.second->_stream.isOpen())
            {
                continue;
            }

            endpointMessage.clear();
            api::DataChannelMessage::makeEndpointMessage(endpointMessage,
                dataStreamEntry.second->_endpointId,
                fromDataStreamItr->second->_endpointId,
                message);

            dataStreamEntry.second->_stream.sendString(endpointMessage.get(), endpointMessage.getLength());
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
    logger::debug("SCTP association established", sender->getLoggableId().c_str());

    auto barbellIt = _engineBarbells.find(sender->getEndpointIdHash());
    if (barbellIt != _engineBarbells.cend() && sender->isDtlsClient())
    {
        barbellIt->second->dataChannel.open("barbell");
    }
}

void EngineMixer::onSctpMessage(transport::RtcTransport* sender,
    uint16_t streamId,
    uint16_t streamSequenceNumber,
    uint32_t payloadProtocol,
    const void* data,
    size_t length)
{
    // TODO  parse this with wonderful json parser and figure out if this is a barbell UMM
    // if it is we should handle it locally in onBarbellUserMediaMap
    if (_engineBarbells.contains(sender->getEndpointIdHash()))
    {
        if (length == 0)
        {
            return;
        }
        auto packet = webrtc::makeUniquePacket(streamId, payloadProtocol, data, length, _sendAllocator);
        auto header = reinterpret_cast<webrtc::SctpStreamMessageHeader*>(packet->get());
        const auto s = reinterpret_cast<char*>(header->data());

        const size_t payloadLen = length - (s - reinterpret_cast<const char*>(data));
        auto messageJson = utils::SimpleJson::create(s, payloadLen);
        {
            if (api::DataChannelMessageParser::isUserMediaMap(messageJson) ||
                api::DataChannelMessageParser::isMinUplinkBitrate(messageJson))
            {
                _incomingBarbellSctp.push(IncomingPacketInfo(std::move(packet), sender));
                return;
            }
        }
    }

    EngineMessage::Message message(EngineMessage::Type::SctpMessage);
    auto& sctpMessage = message.command.sctpMessage;
    sctpMessage.mixer = this;
    sctpMessage.endpointIdHash = sender->getEndpointIdHash();
    message.packet = webrtc::makeUniquePacket(streamId, payloadProtocol, data, length, _sendAllocator);
    if (!message.packet)
    {
        logger::error("Unable to allocate sctp message, sender %p, length %lu", _loggableId.c_str(), sender, length);
        return;
    }

    _messageListener.onMessage(std::move(message));
}

void EngineMixer::onRecControlReceived(transport::RecordingTransport* sender,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    auto recordingStreamItr = _engineRecordingStreams.find(sender->getStreamIdHash());
    if (recordingStreamItr == _engineRecordingStreams.end())
    {
        return;
    }

    auto stream = recordingStreamItr->second;
    auto recControlHeader = recp::RecControlHeader::fromPacket(*packet);
    if (recControlHeader->isEventAck())
    {
        auto unackedPacketsTrackerItr = stream->_recEventUnackedPacketsTracker.find(sender->getEndpointIdHash());
        if (unackedPacketsTrackerItr == stream->_recEventUnackedPacketsTracker.end())
        {
            return;
        }

        sender->getJobQueue().addJob<RecordingEventAckReceiveJob>(std::move(packet),
            sender,
            unackedPacketsTrackerItr->second);
    }
    else if (recControlHeader->isRtpNack())
    {
        auto ssrc = recControlHeader->getSsrc();
        auto ssrcContext = stream->_ssrcOutboundContexts.getItem(ssrc);
        if (!ssrcContext)
        {
            return;
        }

        sender->getJobQueue().addJob<RecordingRtpNackReceiveJob>(std::move(packet),
            _sendAllocator,
            sender,
            *ssrcContext);
    }
}

bool EngineMixer::setPacketSourceEndpointIdHash(memory::Packet& packet,
    size_t barbellIdHash,
    uint32_t ssrc,
    bool isAudio)
{
    auto barbellIt = _engineBarbells.find(barbellIdHash);
    if (barbellIt == _engineBarbells.end())
    {
        return false;
    }

    auto& barbell = *barbellIt->second;
    if (isAudio)
    {
        auto streamIt = barbell.audioSsrcMap.find(ssrc);
        if (streamIt == barbell.audioSsrcMap.end())
        {
            return false;
        }
        auto* audioStream = streamIt->second;
        if (audioStream->endpointIdHash.isSet())
        {
            packet.endpointIdHash = audioStream->endpointIdHash.get();
            return true;
        }
    }
    else
    {
        auto streamIt = barbell.videoSsrcMap.find(ssrc);
        if (streamIt == barbell.videoSsrcMap.end())
        {
            return false;
        }
        auto* videoStream = streamIt->second;
        if (videoStream->endpointIdHash.isSet())
        {
            packet.endpointIdHash = videoStream->endpointIdHash.get();
            return true;
        }
    }

    return false;
}

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
        return;
    }

    if (EngineBarbell::isFromBarbell(sender->getTag()))
    {
        const bool isAudio = (ssrcContext->_rtpMap._format == bridge::RtpMap::Format::OPUS);
        if (!setPacketSourceEndpointIdHash(*packet, sender->getEndpointIdHash(), ssrc, isAudio))
        {
            logger::debug("incoming barbell packet unmapped %zu", _loggableId.c_str(), sender->getEndpointIdHash());
            return; // drop packet as we cannot process it
        }
    }

    switch (ssrcContext->_rtpMap._format)
    {
    case bridge::RtpMap::Format::OPUS:
        ssrcContext->onRtpPacket(timestamp);
        if (_engineAudioStreams.size() > 0)
        {
            sender->getJobQueue().addJob<bridge::AudioForwarderReceiveJob>(std::move(packet),
                sender,
                *this,
                *ssrcContext,
                *_activeMediaList,
                _config.audio.silenceThresholdLevel,
                _numMixedAudioStreams != 0,
                extendedSequenceNumber);
        }
        break;

    case bridge::RtpMap::Format::VP8:
        onVideoRtpPacketReceived(ssrcContext, sender, std::move(packet), extendedSequenceNumber, timestamp);
        break;

    case bridge::RtpMap::Format::VP8RTX:
        ssrcContext->onRtpPacket(timestamp);
        onVideoRtpRtxPacketReceived(ssrcContext, sender, std::move(packet), extendedSequenceNumber, timestamp);
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
    auto ssrcOutboundContextItr = ssrcOutboundContexts.find(ssrc);
    if (ssrcOutboundContextItr != ssrcOutboundContexts.cend())
    {
        return &ssrcOutboundContextItr->second;
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

SsrcInboundContext* EngineMixer::emplaceInboundSsrcContext(const uint32_t ssrc,
    transport::RtcTransport* sender,
    const uint32_t payloadType,
    const uint64_t timestamp)
{
    auto ssrcInboundContextsItr = _ssrcInboundContexts.find(ssrc);
    if (ssrcInboundContextsItr != _ssrcInboundContexts.end())
    {
        return &ssrcInboundContextsItr->second;
    }

    const auto endpointIdHash = sender->getEndpointIdHash();
    auto* audioStream = _engineAudioStreams.getItem(endpointIdHash);

    if (audioStream && audioStream->rtpMap._payloadType == payloadType)
    {
        if (!audioStream->remoteSsrc.isSet() || audioStream->remoteSsrc.get() != ssrc)
        {
            return nullptr;
        }

        auto emplaceResult = _ssrcInboundContexts.emplace(ssrc, ssrc, audioStream->rtpMap, sender, timestamp);

        if (!emplaceResult.second && emplaceResult.first == _ssrcInboundContexts.end())
        {
            logger::error("Failed to create inbound context for ssrc %u", _loggableId.c_str(), ssrc);
            return nullptr;
        }

        logger::info("Created new inbound context for audio stream ssrc %u, endpointIdHash %lu, audioLevelExtId %d, "
                     "absSendTimeExtId %d, %s",
            _loggableId.c_str(),
            ssrc,
            audioStream->endpointIdHash,
            audioStream->rtpMap._audioLevelExtId.isSet() ? audioStream->rtpMap._audioLevelExtId.get() : -1,
            audioStream->rtpMap._absSendTimeExtId.isSet() ? audioStream->rtpMap._absSendTimeExtId.get() : -1,
            sender->getLoggableId().c_str());
        return &emplaceResult.first->second;
    }

    auto* barbell = _engineBarbells.getItem(endpointIdHash);
    if (barbell)
    {
        if (barbell->videoSsrcMap.contains(ssrc))
        {
            const RtpMap& videoRtpMap =
                barbell->videoRtpMap._payloadType == payloadType ? barbell->videoRtpMap : barbell->videoFeedbackRtpMap;

            auto emplaceResult = _ssrcInboundContexts.emplace(ssrc, ssrc, videoRtpMap, sender, timestamp);

            if (!emplaceResult.second && emplaceResult.first == _ssrcInboundContexts.end())
            {
                logger::error("Failed to create barbell inbound video context for ssrc %u", _loggableId.c_str(), ssrc);
                return nullptr;
            }
            auto& inboundContext = emplaceResult.first->second;
            if (&videoRtpMap == &barbell->videoRtpMap)
            {
                inboundContext._rtxSsrc = barbell->getFeedbackSsrcFor(ssrc);
            }
            else
            {
                inboundContext._rtxSsrc = barbell->getMainSsrcFor(ssrc);
            }

            auto videoStream = barbell->videoSsrcMap.getItem(ssrc);
            assert(videoStream);
            inboundContext._simulcastLevel = videoStream->stream.getLevelOf(ssrc);

            logger::info("Created new barbell inbound video context for stream ssrc %u, endpointIdHash %zu, %s",
                _loggableId.c_str(),
                ssrc,
                endpointIdHash,
                sender->getLoggableId().c_str());
            return &emplaceResult.first->second;
        }

        if (barbell->audioSsrcMap.contains(ssrc))
        {
            auto emplaceResult = _ssrcInboundContexts.emplace(ssrc, ssrc, barbell->audioRtpMap, sender, timestamp);

            if (!emplaceResult.second && emplaceResult.first == _ssrcInboundContexts.end())
            {
                logger::error("Failed to create barbell inbound audio context for ssrc %u", _loggableId.c_str(), ssrc);
                return nullptr;
            }

            logger::info("Created new barbelll inbound audio context for stream ssrc %u, endpointIdHash %zu, %s",
                _loggableId.c_str(),
                ssrc,
                endpointIdHash,
                sender->getLoggableId().c_str());
            return &emplaceResult.first->second;
        }
        return nullptr;
    }

    const auto engineVideoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (engineVideoStreamItr == _engineVideoStreams.end())
    {
        return nullptr;
    }

    auto videoStream = engineVideoStreamItr->second;
    if (payloadType == videoStream->_rtpMap._payloadType)
    {
        uint32_t rewriteSsrc = 0;
        uint32_t level = 0;
        for (size_t i = 0; i < videoStream->_simulcastStream._numLevels; ++i)
        {
            if (ssrc == videoStream->_simulcastStream._levels[i]._ssrc)
            {
                level = i;
                rewriteSsrc = videoStream->_simulcastStream._levels[0]._ssrc;
                break;
            }
        }

        if (rewriteSsrc == 0 && videoStream->_secondarySimulcastStream.isSet())
        {
            for (size_t i = 0; i < videoStream->_secondarySimulcastStream.get()._numLevels; ++i)
            {
                if (ssrc == videoStream->_secondarySimulcastStream.get()._levels[i]._ssrc)
                {
                    level = i;
                    rewriteSsrc = videoStream->_secondarySimulcastStream.get()._levels[0]._ssrc;
                    break;
                }
            }
        }

        if (rewriteSsrc == 0)
        {
            return nullptr;
        }

        auto emplaceResult = _ssrcInboundContexts.emplace(ssrc, ssrc, videoStream->_rtpMap, sender, timestamp);
        auto& inboundContext = emplaceResult.first->second;
        inboundContext._rewriteSsrc = rewriteSsrc;
        inboundContext._simulcastLevel = level;
        inboundContext._rtxSsrc = videoStream->getFeedbackSsrcFor(ssrc);

        logger::info(
            "Created new inbound context for video stream ssrc %u, level %u, rewrite ssrc %u, endpointIdHash %lu, rtp "
            "format %u, %s",
            _loggableId.c_str(),
            ssrc,
            level,
            inboundContext._rewriteSsrc,
            videoStream->_endpointIdHash,
            static_cast<uint16_t>(inboundContext._rtpMap._format),
            sender->getLoggableId().c_str());

        return &inboundContext;
    }
    else if (payloadType == videoStream->_feedbackRtpMap._payloadType)
    {
        uint32_t rewriteSsrc = 0;
        for (size_t i = 0; i < videoStream->_simulcastStream._numLevels; ++i)
        {
            if (ssrc == videoStream->_simulcastStream._levels[i]._feedbackSsrc)
            {
                rewriteSsrc = videoStream->_simulcastStream._levels[0]._feedbackSsrc;
                break;
            }
        }

        if (rewriteSsrc == 0 && videoStream->_secondarySimulcastStream.isSet())
        {
            for (size_t i = 0; i < videoStream->_secondarySimulcastStream.get()._numLevels; ++i)
            {
                if (ssrc == videoStream->_secondarySimulcastStream.get()._levels[i]._feedbackSsrc)
                {
                    rewriteSsrc = videoStream->_secondarySimulcastStream.get()._levels[0]._feedbackSsrc;
                    break;
                }
            }
        }

        if (rewriteSsrc == 0)
        {
            return nullptr;
        }

        auto emplaceResult = _ssrcInboundContexts.emplace(ssrc, ssrc, videoStream->_feedbackRtpMap, sender, timestamp);
        auto& inboundContext = emplaceResult.first->second;
        inboundContext._rewriteSsrc = rewriteSsrc;
        inboundContext._rtxSsrc = videoStream->getMainSsrcFor(ssrc);

        logger::debug(
            "Created new inbound context for video stream feedback ssrc %u, endpointIdHash %lu, rtp format %u, %s",
            _loggableId.c_str(),
            ssrc,
            videoStream->_endpointIdHash,
            static_cast<uint16_t>(inboundContext._rtpMap._format),
            sender->getLoggableId().c_str());

        return &inboundContext;
    }

    return nullptr;
}

void EngineMixer::processBarbellSctp(const uint64_t timestamp)
{
    for (IncomingPacketInfo packetInfo; _incomingBarbellSctp.pop(packetInfo);)
    {
        auto header = reinterpret_cast<webrtc::SctpStreamMessageHeader*>(packetInfo.packet()->get());
        auto message = reinterpret_cast<const char*>(header->data());
        auto messageJson = utils::SimpleJson::create(message, strlen(message));

        if (api::DataChannelMessageParser::isUserMediaMap(messageJson))
        {
            return onBarbellUserMediaMap(packetInfo.transport()->getEndpointIdHash(), message);
        }

        if (api::DataChannelMessageParser::isMinUplinkBitrate(messageJson))
        {
            return onBarbellMinUplinkEstimate(packetInfo.transport()->getEndpointIdHash(), message);
        }
    }
}

void EngineMixer::forwardAudioRtpPacket(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    for (auto& audioStreamEntry : _engineAudioStreams)
    {
        auto audioStream = audioStreamEntry.second;
        if (!audioStream || &audioStream->transport == packetInfo.transport() || audioStream->audioMixed)
        {
            continue;
        }

        if (audioStream->transport.isConnected())
        {
            auto ssrc = packetInfo.inboundContext()->_ssrc;
            if (audioStream->ssrcRewrite)
            {
                const auto& audioSsrcRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
                const auto rewriteMapItr = audioSsrcRewriteMap.find(packetInfo.packet()->endpointIdHash);
                if (rewriteMapItr == audioSsrcRewriteMap.end())
                {
                    continue;
                }
                ssrc = rewriteMapItr->second;
            }

            auto* ssrcOutboundContext = obtainOutboundSsrcContext(audioStream->endpointIdHash,
                audioStream->ssrcOutboundContexts,
                ssrc,
                audioStream->rtpMap);

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
}

void EngineMixer::forwardAudioRtpPacketRecording(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    for (auto& recordingStreams : _engineRecordingStreams)
    {
        auto* recordingStream = recordingStreams.second;
        if (!(recordingStream && recordingStream->_isAudioEnabled))
        {
            continue;
        }

        const auto ssrc = packetInfo.inboundContext()->_ssrc;
        auto* ssrcOutboundContext = recordingStream->_ssrcOutboundContexts.getItem(ssrc);
        if (!ssrcOutboundContext || ssrcOutboundContext->_markedForDeletion)
        {
            continue;
        }

        allocateRecordingRtpPacketCacheIfNecessary(*ssrcOutboundContext, *recordingStream);

        for (const auto& transportEntry : recordingStream->_transports)
        {
            ssrcOutboundContext->onRtpSent(timestamp);
            auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
            if (packet)
            {
                transportEntry.second.getJobQueue().addJob<RecordingAudioForwarderSendJob>(*ssrcOutboundContext,
                    std::move(packet),
                    transportEntry.second,
                    packetInfo.extendedSequenceNumber());
            }
            else
            {
                logger::warn("send allocator depleted RecFwdSend", _loggableId.c_str());
            }
        }
    }
}

void EngineMixer::forwardAudioRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    for (auto& it : _engineBarbells)
    {
        auto& barbell = *it.second;
        if (&barbell.transport == packetInfo.transport())
        {
            continue; // not sending back over same barbell
        }

        const auto& audioSsrcRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
        const auto rewriteMapItr = audioSsrcRewriteMap.find(packetInfo.packet()->endpointIdHash);
        if (rewriteMapItr == audioSsrcRewriteMap.end())
        {
            continue;
        }
        uint32_t ssrc = rewriteMapItr->second;

        auto* ssrcOutboundContext =
            obtainOutboundSsrcContext(barbell.idHash, barbell.ssrcOutboundContexts, ssrc, barbell.audioRtpMap);

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
                barbell.transport);
        }
        else
        {
            logger::warn("send allocator depleted. forwardAudioRtpPacketOverBarbell", _loggableId.c_str());
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

        forwardAudioRtpPacket(packetInfo, timestamp);
        forwardAudioRtpPacketRecording(packetInfo, timestamp);
        forwardAudioRtpPacketOverBarbell(packetInfo, timestamp);
    }

    for (IncomingPacketInfo packetInfo; _incomingForwarderVideoRtp.pop(packetInfo);)
    {
        ++numRtpPackets;
        forwardVideoRtpPacket(packetInfo, timestamp);
        forwardVideoRtpPacketOverBarbell(packetInfo, timestamp);
        forwardVideoRtpPacketRecording(packetInfo, timestamp);
    }

    bool overrunLogSpamGuard = false;

    for (IncomingAudioPacketInfo packetInfo; _incomingMixerAudioRtp.pop(packetInfo);)
    {
        ++numRtpPackets;

        const auto rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
        if (!rtpHeader)
        {
            continue;
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
            {
                EngineMessage::Message message(EngineMessage::Type::AllocateAudioBuffer);
                message.command.allocateAudioBuffer.mixer = this;
                message.command.allocateAudioBuffer.ssrc = ssrc.get();
                _messageListener.onMessage(std::move(message));
            }
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

    if (numRtpPackets == 0)
    {
        if (utils::Time::diffGE(_lastReceiveTime, timestamp, _config.mixerInactivityTimeoutMs * utils::Time::ms))
        {
            EngineMessage::Message message(EngineMessage::Type::MixerTimedOut);
            message.command.mixerTimedOut.mixer = this;
            _messageListener.onMessage(std::move(message));
        }
    }
    else
    {
        _lastReceiveTime = timestamp;
    }
}

void EngineMixer::forwardVideoRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    const auto senderEndpointIdHash = packetInfo.packet()->endpointIdHash;
    for (auto& it : _engineBarbells)
    {
        auto& barbell = *it.second;
        if (&barbell.transport == packetInfo.transport() || !packetInfo.inboundContext())
        {
            continue; // not sending back over same barbell
        }

        auto ssrc = packetInfo.inboundContext()->_rewriteSsrc;
        auto simulcastLevel = packetInfo.inboundContext()->_simulcastLevel;
        const auto& screenShareSsrcMapping = _activeMediaList->getVideoScreenShareSsrcMapping();
        if (screenShareSsrcMapping.isSet() && screenShareSsrcMapping.get().first == senderEndpointIdHash &&
            screenShareSsrcMapping.get().second._ssrc == ssrc)
        {
            ssrc = screenShareSsrcMapping.get().second._rewriteSsrc;
        }
        else
        {
            const auto& videoSsrcRewriteMap = _activeMediaList->getVideoSsrcRewriteMap();
            const auto* rewriteMapping = videoSsrcRewriteMap.getItem(senderEndpointIdHash);
            if (!rewriteMapping)
            {
                continue;
            }

            ssrc = (*rewriteMapping)[simulcastLevel].main;
        }

        auto* ssrcOutboundContext =
            obtainOutboundSsrcContext(barbell.idHash, barbell.ssrcOutboundContexts, ssrc, barbell.videoRtpMap);
        if (!ssrcOutboundContext)
        {
            continue;
        }

        if (!ssrcOutboundContext->_packetCache.isSet())
        {
            logger::debug("New ssrc %u seen, sending request to add videoPacketCache to barbell",
                _loggableId.c_str(),
                ssrc);

            ssrcOutboundContext->_packetCache.set(nullptr);
            {
                EngineMessage::Message message(EngineMessage::Type::AllocateVideoPacketCache);
                message.command.allocateVideoPacketCache.mixer = this;
                message.command.allocateVideoPacketCache.ssrc = ssrc;
                message.command.allocateVideoPacketCache.endpointIdHash = barbell.idHash;
                _messageListener.onMessage(std::move(message));
            }
        }

        ssrcOutboundContext->onRtpSent(timestamp); // marks that we have active jobs on this ssrc context
        auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
        if (packet)
        {
            barbell.transport.getJobQueue().addJob<VideoForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                *(packetInfo.inboundContext()),
                std::move(packet),
                barbell.transport,
                packetInfo.extendedSequenceNumber());
        }
        else
        {
            logger::warn("send allocator depleted fwdVideoOverBarbell", _loggableId.c_str());
        }
    }
}

void EngineMixer::forwardVideoRtpPacket(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*packetInfo.packet());
    if (!rtpHeader)
    {
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

        if (!_engineStreamDirector->shouldForwardSsrc(endpointIdHash, packetInfo.inboundContext()->_ssrc))
        {
            continue;
        }

        if (shouldSkipBecauseOfWhitelist(*videoStream, packetInfo.inboundContext()->_ssrc))
        {
            continue;
        }

        auto ssrc = packetInfo.inboundContext()->_rewriteSsrc;
        if (videoStream->_ssrcRewrite)
        {
            const auto& screenShareSsrcMapping = _activeMediaList->getVideoScreenShareSsrcMapping();
            if (screenShareSsrcMapping.isSet() && screenShareSsrcMapping.get().first == senderEndpointIdHash &&
                screenShareSsrcMapping.get().second._ssrc == ssrc)
            {
                ssrc = screenShareSsrcMapping.get().second._rewriteSsrc;
            }
            else if (_engineStreamDirector->getPinTarget(endpointIdHash) == senderEndpointIdHash &&
                !_activeMediaList->isInUserActiveVideoList(senderEndpointIdHash))
            {
                if (videoStream->_pinSsrc.isSet())
                {
                    ssrc = videoStream->_pinSsrc.get()._ssrc;
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
                const auto rewriteMapItr = videoSsrcRewriteMap.find(senderEndpointIdHash);
                if (rewriteMapItr == videoSsrcRewriteMap.end())
                {
                    continue;
                }
                ssrc = rewriteMapItr->second[0].main;
            }
        }
        else
        {
            uint32_t fbSsrc = 0;
            _engineStreamDirector->getFeedbackSsrc(ssrc, fbSsrc);
        }

        auto* ssrcOutboundContext = obtainOutboundSsrcContext(videoStream->_endpointIdHash,
            videoStream->_ssrcOutboundContexts,
            ssrc,
            videoStream->_rtpMap);

        if (!ssrcOutboundContext)
        {
            continue;
        }

        if (!ssrcOutboundContext->_packetCache.isSet())
        {
            logger::debug("New ssrc %u seen, sending request to add videoPacketCache", _loggableId.c_str(), ssrc);

            ssrcOutboundContext->_packetCache.set(nullptr);
            {
                EngineMessage::Message message(EngineMessage::Type::AllocateVideoPacketCache);
                message.command.allocateVideoPacketCache.mixer = this;
                message.command.allocateVideoPacketCache.ssrc = ssrc;
                message.command.allocateVideoPacketCache.endpointIdHash = endpointIdHash;
                _messageListener.onMessage(std::move(message));
            }
        }

        if (videoStream->_transport.isConnected())
        {
            ssrcOutboundContext->onRtpSent(timestamp); // marks that we have active jobs on this ssrc context
            auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
            if (packet)
            {
                videoStream->_transport.getJobQueue().addJob<VideoForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                    *(packetInfo.inboundContext()),
                    std::move(packet),
                    videoStream->_transport,
                    packetInfo.extendedSequenceNumber());
            }
            else
            {
                logger::warn("send allocator depleted FwdRewrite", _loggableId.c_str());
            }
        }
    }
}

void EngineMixer::forwardVideoRtpPacketRecording(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    for (auto& recordingStreams : _engineRecordingStreams)
    {
        auto* recordingStream = recordingStreams.second;
        if (!(recordingStream && (recordingStream->_isVideoEnabled || recordingStream->_isScreenSharingEnabled)))
        {
            continue;
        }

        if (!_engineStreamDirector->shouldForwardSsrc(recordingStream->_endpointIdHash,
                packetInfo.inboundContext()->_ssrc))
        {
            continue;
        }

        auto* ssrcOutboundContext =
            recordingStream->_ssrcOutboundContexts.getItem(packetInfo.inboundContext()->_rewriteSsrc);
        if (!ssrcOutboundContext || ssrcOutboundContext->_markedForDeletion)
        {
            continue;
        }

        allocateRecordingRtpPacketCacheIfNecessary(*ssrcOutboundContext, *recordingStream);

        for (const auto& transportEntry : recordingStream->_transports)
        {
            ssrcOutboundContext->onRtpSent(timestamp); // active jobs on this ssrc context
            auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
            if (packet)
            {
                transportEntry.second.getJobQueue().addJob<VideoForwarderRewriteAndSendJob>(*ssrcOutboundContext,
                    *(packetInfo.inboundContext()),
                    std::move(packet),
                    transportEntry.second,
                    packetInfo.extendedSequenceNumber());
            }
            else
            {
                logger::warn("send allocator depleted FwdRewrite", _loggableId.c_str());
            }
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
                processIncomingPayloadSpecificRtcpPacket(packetInfo.transport()->getEndpointIdHash(), rtcpPacket);
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
    const rtp::RtcpHeader& rtcpPacket)
{
    auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
    if (rtcpFeedback->_header.fmtCount != rtp::PayloadSpecificFeedbackType::Pli &&
        rtcpFeedback->_header.fmtCount != rtp::PayloadSpecificFeedbackType::Fir)
    {
        return;
    }

    const auto& reverseRewriteMap = _activeMediaList->getReverseVideoSsrcRewriteMap();
    const auto reverseRewriteMapItr = reverseRewriteMap.find(rtcpFeedback->_mediaSsrc.get());
    const auto& videoScreenShareSsrcMapping = _activeMediaList->getVideoScreenShareSsrcMapping();
    const auto mediaSsrc = rtcpFeedback->_mediaSsrc.get();
    const auto rtcpSenderVideoStreamItr = _engineVideoStreams.find(rtcpSenderEndpointIdHash);

    size_t participant;
    if (rtcpSenderVideoStreamItr != _engineVideoStreams.end() && rtcpSenderVideoStreamItr->second->_pinSsrc.isSet() &&
        rtcpSenderVideoStreamItr->second->_pinSsrc.get()._ssrc == mediaSsrc)
    {
        // The mediaSsrc refers to the pinned video ssrc
        participant = _engineStreamDirector->getPinTarget(rtcpSenderEndpointIdHash);
    }
    else if (videoScreenShareSsrcMapping.isSet() && videoScreenShareSsrcMapping.get().second._rewriteSsrc == mediaSsrc)
    {
        // The mediaSsrc refers to the screen share ssrc
        participant = videoScreenShareSsrcMapping.get().first;
    }
    else if (reverseRewriteMapItr != reverseRewriteMap.end())
    {
        // The mediaSsrc refers to a rewritten ssrc
        participant = reverseRewriteMapItr->second;
    }
    else
    {
        // The mediaSsrc is not rewritten
        participant = _engineStreamDirector->getParticipantForDefaultLevelSsrc(rtcpFeedback->_mediaSsrc.get());
    }

    if (!participant)
    {
        return;
    }

    auto videoStreamItr = _engineVideoStreams.find(participant);
    if (videoStreamItr == _engineVideoStreams.end())
    {
        return;
    }

    if (videoStreamItr->second->_localSsrc == rtcpFeedback->_mediaSsrc.get())
    {
        return;
    }

    logger::info("Incoming rtcp feedback PLI, reporterSsrc %u, mediaSsrc %u, reporter participant %zu, media "
                 "participant %zu",
        _loggableId.c_str(),
        rtcpFeedback->_reporterSsrc.get(),
        rtcpFeedback->_mediaSsrc.get(),
        rtcpSenderEndpointIdHash,
        participant);

    sendPliForUsedSsrcs(*videoStreamItr->second);
}

void EngineMixer::processIncomingTransportFbRtcpPacket(const transport::RtcTransport* transport,
    const rtp::RtcpHeader& rtcpPacket,
    const uint64_t timestamp)
{
    auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
    if (rtcpFeedback->_header.fmtCount != rtp::TransportLayerFeedbackType::PacketNack)
    {
        return;
    }

    const auto mediaSsrc = rtcpFeedback->_mediaSsrc.get();

    auto rtcpSenderVideoStreamItr = _engineVideoStreams.find(transport->getEndpointIdHash());
    if (rtcpSenderVideoStreamItr == _engineVideoStreams.end())
    {
        return;
    }
    auto rtcpSenderVideoStream = rtcpSenderVideoStreamItr->second;

    auto* mediaSsrcOutboundContext = rtcpSenderVideoStream->_ssrcOutboundContexts.getItem(mediaSsrc);
    if (!mediaSsrcOutboundContext || !mediaSsrcOutboundContext->_packetCache.isSet() ||
        !mediaSsrcOutboundContext->_packetCache.get())
    {
        return;
    }

    uint32_t feedbackSsrc;
    if (!(rtcpSenderVideoStream->_ssrcRewrite ? _activeMediaList->getFeedbackSsrc(mediaSsrc, feedbackSsrc)
                                              : _engineStreamDirector->getFeedbackSsrc(mediaSsrc, feedbackSsrc)))
    {
        return;
    }

    auto feedbackSsrcOutboundContext = obtainOutboundSsrcContext(rtcpSenderVideoStream->_endpointIdHash,
        rtcpSenderVideoStream->_ssrcOutboundContexts,
        feedbackSsrc,
        rtcpSenderVideoStream->_feedbackRtpMap);

    if (!feedbackSsrcOutboundContext)
    {
        return;
    }

    mediaSsrcOutboundContext->onRtpSent(timestamp);
    const auto numFeedbackControlInfos = rtp::getNumFeedbackControlInfos(rtcpFeedback);
    uint16_t pid = 0;
    uint16_t blp = 0;
    for (size_t i = 0; i < numFeedbackControlInfos; ++i)
    {
        feedbackSsrcOutboundContext->onRtpSent(timestamp);
        rtp::getFeedbackControlInfo(rtcpFeedback, i, numFeedbackControlInfos, pid, blp);
        rtcpSenderVideoStream->_transport.getJobQueue().addJob<bridge::VideoNackReceiveJob>(
            *feedbackSsrcOutboundContext,
            rtcpSenderVideoStream->_transport,
            *(mediaSsrcOutboundContext->_packetCache.get()),
            pid,
            blp,
            feedbackSsrc,
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
            auto mixerAudioBufferItr = _mixerSsrcAudioBuffers.find(audioStream->remoteSsrc.get());
            if (mixerAudioBufferItr != _mixerSsrcAudioBuffers.end())
            {
                audioBuffer = mixerAudioBufferItr->second;
            }

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

void EngineMixer::sendPliForUsedSsrcs(EngineVideoStream& videoStream)
{
    const auto isSenderInLastNList = _activeMediaList->isInActiveVideoList(videoStream._endpointIdHash);

    for (size_t i = 0; i < videoStream._simulcastStream._numLevels; ++i)
    {
        const auto& simulcastLevel = videoStream._simulcastStream._levels[i];
        if (!_engineStreamDirector->isSsrcUsed(simulcastLevel._ssrc,
                videoStream._endpointIdHash,
                isSenderInLastNList,
                _engineRecordingStreams.size()))
        {
            continue;
        }
        auto ssrcIt = _ssrcInboundContexts.find(simulcastLevel._ssrc);
        if (ssrcIt != _ssrcInboundContexts.end())
        {
            logger::debug("RequestPliJob created for inbound ssrc %u", _loggableId.c_str(), ssrcIt->second._ssrc);
            ssrcIt->second._pliScheduler.triggerPli();
        }
    }

    if (videoStream._secondarySimulcastStream.isSet())
    {
        for (size_t i = 0; i < videoStream._secondarySimulcastStream.get()._numLevels; ++i)
        {
            const auto& simulcastLevel = videoStream._secondarySimulcastStream.get()._levels[i];
            if (!_engineStreamDirector->isSsrcUsed(simulcastLevel._ssrc,
                    videoStream._endpointIdHash,
                    isSenderInLastNList,
                    _engineRecordingStreams.size()))
            {
                continue;
            }
            auto ssrcIt = _ssrcInboundContexts.find(simulcastLevel._ssrc);
            if (ssrcIt != _ssrcInboundContexts.end())
            {
                ssrcIt->second._pliScheduler.triggerPli();
            }
        }
    }
}

void EngineMixer::sendLastNListMessage(const size_t endpointIdHash)
{
    utils::StringBuilder<1024> lastNListMessage;
    auto dataStreamItr = _engineDataStreams.find(endpointIdHash);
    if (dataStreamItr == _engineDataStreams.end())
    {
        return;
    }
    auto dataStream = dataStreamItr->second;
    if (!dataStream->_stream.isOpen() || !dataStream->_hasSeenInitialSpeakerList)
    {
        return;
    }

    const auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr == _engineVideoStreams.end() || videoStreamItr->second->_ssrcRewrite)
    {
        return;
    }

    auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
    _activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage);

    dataStream->_stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
}

void EngineMixer::sendLastNListMessageToAll()
{
    utils::StringBuilder<1024> lastNListMessage;

    for (auto& dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (!dataStream->_stream.isOpen() || !dataStream->_hasSeenInitialSpeakerList)
        {
            continue;
        }

        const auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
        if (videoStreamItr == _engineVideoStreams.end() || videoStreamItr->second->_ssrcRewrite)
        {
            continue;
        }

        lastNListMessage.clear();
        auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
        _activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage);

        dataStream->_stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
    }
}

void EngineMixer::sendMessagesToNewDataStreams()
{
    const auto dominantSpeakerParticipant = _activeMediaList->getDominantSpeaker();
    auto dominantSpeakerVideoStreamItr = _engineVideoStreams.find(dominantSpeakerParticipant);

    utils::StringBuilder<256> dominantSpeakerMessage;
    if (dominantSpeakerVideoStreamItr != _engineVideoStreams.end())
    {
        api::DataChannelMessage::makeDominantSpeaker(dominantSpeakerMessage,
            dominantSpeakerVideoStreamItr->second->_endpointId);
    }

    utils::StringBuilder<1024> lastNListMessage;
    utils::StringBuilder<1024> userMediaMapMessage;

    for (auto& dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (dataStream->_hasSeenInitialSpeakerList || !dataStream->_stream.isOpen())
        {
            continue;
        }

        dataStream->_stream.sendString(dominantSpeakerMessage.get(), dominantSpeakerMessage.getLength());

        const auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
        if (videoStreamItr == _engineVideoStreams.end())
        {
            continue;
        }
        const auto videoStream = videoStreamItr->second;
        const auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);

        if (videoStream->_ssrcRewrite)
        {
            userMediaMapMessage.clear();
            if (_activeMediaList->makeUserMediaMapMessage(_lastN,
                    dataStreamEntry.first,
                    pinTarget,
                    _engineVideoStreams,
                    userMediaMapMessage))
            {
                dataStream->_stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
            }
        }
        else
        {
            lastNListMessage.clear();
            if (_activeMediaList->makeLastNListMessage(_lastN, endpointIdHash, pinTarget, lastNListMessage))
            {
                dataStream->_stream.sendString(lastNListMessage.get(), lastNListMessage.getLength());
            }
        }

        dataStream->_hasSeenInitialSpeakerList = true;
    }
}

void EngineMixer::updateBandwidthFloor()
{
    auto videoStreams = 0;
    for (const auto& videoStreamEntry : _engineVideoStreams)
    {
        if (videoStreamEntry.second->_simulcastStream._numLevels > 0)
        {
            ++videoStreams;
        }
        if (videoStreamEntry.second->_secondarySimulcastStream.isSet() &&
            videoStreamEntry.second->_secondarySimulcastStream.get()._numLevels > 0)
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

    const auto dataStreamItr = _engineDataStreams.find(endpointIdHash);
    if (dataStreamItr == _engineDataStreams.end())
    {
        return;
    }

    auto dataStream = dataStreamItr->second;
    if (!dataStream->_stream.isOpen() || !dataStream->_hasSeenInitialSpeakerList)
    {
        return;
    }

    const auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
    if (videoStreamItr == _engineVideoStreams.end() || !videoStreamItr->second->_ssrcRewrite)
    {
        return;
    }

    const auto pinTarget = _engineStreamDirector->getPinTarget(endpointIdHash);
    _activeMediaList->makeUserMediaMapMessage(_lastN,
        endpointIdHash,
        pinTarget,
        _engineVideoStreams,
        userMediaMapMessage);

    dataStream->_stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
}

void EngineMixer::sendUserMediaMapMessageToAll()
{
    utils::StringBuilder<1024> userMediaMapMessage;
    for (auto dataStreamEntry : _engineDataStreams)
    {
        const auto endpointIdHash = dataStreamEntry.first;
        auto dataStream = dataStreamEntry.second;
        if (!dataStream->_stream.isOpen() || !dataStream->_hasSeenInitialSpeakerList)
        {
            continue;
        }

        const auto videoStreamItr = _engineVideoStreams.find(endpointIdHash);
        if (videoStreamItr == _engineVideoStreams.end() || !videoStreamItr->second->_ssrcRewrite)
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

        dataStream->_stream.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
    }
}

void EngineMixer::sendUserMediaMapMessageOverBarbells()
{
    if (_engineBarbells.size() == 0)
    {
        return;
    }

    utils::StringBuilder<1024> userMediaMapMessage;
    _activeMediaList->makeBarbellUserMediaMapMessage(userMediaMapMessage);

    for (auto& barbell : _engineBarbells)
    {
        logger::debug("send BB msg %s", _loggableId.c_str(), userMediaMapMessage.get());
        barbell.second->dataChannel.sendString(userMediaMapMessage.get(), userMediaMapMessage.getLength());
    }
}

void EngineMixer::sendDominantSpeakerMessageToAll(const size_t dominantSpeaker)
{
    auto dominantSpeakerDataStreamItr = _engineDataStreams.find(dominantSpeaker);
    if (dominantSpeakerDataStreamItr == _engineDataStreams.end())
    {
        return;
    }

    utils::StringBuilder<256> dominantSpeakerMessage;
    api::DataChannelMessage::makeDominantSpeaker(dominantSpeakerMessage,
        dominantSpeakerDataStreamItr->second->_endpointId);

    for (auto dataStreamEntry : _engineDataStreams)
    {
        auto dataStream = dataStreamEntry.second;
        if (!dataStream->_stream.isOpen() || !dataStream->_hasSeenInitialSpeakerList)
        {
            continue;
        }
        dataStream->_stream.sendString(dominantSpeakerMessage.get(), dominantSpeakerMessage.getLength());
    }

    for (auto& recStreamPair : _engineRecordingStreams)
    {
        sendDominantSpeakerToRecordingStream(*recStreamPair.second,
            dominantSpeaker,
            dominantSpeakerDataStreamItr->second->_endpointId);
    }
}

void EngineMixer::sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream,
    const size_t dominantSpeaker,
    const std::string& dominantSpeakerEndpoint)
{
    if (recordingStream._isVideoEnabled)
    {
        pinEndpoint(recordingStream._endpointIdHash, dominantSpeaker);
    }

    const auto sequenceNumber = recordingStream._recordingEventsOutboundContext._sequenceNumber++;
    const auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : recordingStream._transports)
    {
        auto unackedPacketsTrackerItr = recordingStream._recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == recordingStream._recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send dominant speaker recording event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        auto packet = recp::RecDominantSpeakerEventBuilder(_sendAllocator)
                          .setSequenceNumber(sequenceNumber)
                          .setTimestamp(timestamp)
                          .setDominantSpeakerEndpoint(dominantSpeakerEndpoint)
                          .build();

        if (!packet)
        {
            logger::warn("No space available to allocate rec dominant speaker event", _loggableId.c_str());
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            recordingStream._recordingEventsOutboundContext._packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream)
{
    const auto dominantSpeaker = _activeMediaList->getDominantSpeaker();
    auto audioStreamsItr = _engineAudioStreams.find(dominantSpeaker);
    if (audioStreamsItr != _engineAudioStreams.end())
    {
        sendDominantSpeakerToRecordingStream(recordingStream, dominantSpeaker, audioStreamsItr->second->endpointId);
    }
}

void EngineMixer::updateSimulcastLevelActiveState(EngineVideoStream& videoStream,
    const SimulcastStream& simulcastStream)
{
    for (size_t i = 0; i < simulcastStream._numLevels; ++i)
    {
        const auto ssrc = simulcastStream._levels[i]._ssrc;
        auto ssrcInboundContextItr = _ssrcInboundContexts.find(ssrc);
        if (ssrcInboundContextItr != _ssrcInboundContexts.end() && ssrcInboundContextItr->second._activeMedia)
        {
            _engineStreamDirector->streamActiveStateChanged(videoStream._endpointIdHash, ssrc, true);
            ssrcInboundContextItr->second._pliScheduler.triggerPli();
        }
    }
}

void EngineMixer::markAssociatedVideoOutboundContextsForDeletion(EngineVideoStream* senderVideoStream,
    const uint32_t ssrc,
    const uint32_t feedbackSsrc)
{
    for (auto& videoStreamEntry : _engineVideoStreams)
    {
        auto videoStream = videoStreamEntry.second;
        if (videoStream == senderVideoStream)
        {
            continue;
        }
        const auto endpointIdHash = videoStreamEntry.first;

        {
            auto outboundContextItr = videoStream->_ssrcOutboundContexts.find(ssrc);
            if (outboundContextItr != videoStream->_ssrcOutboundContexts.end())
            {
                outboundContextItr->second._markedForDeletion = true;
                logger::info("Marking unused video outbound context for deletion, ssrc %u, endpointIdHash %lu",
                    _loggableId.c_str(),
                    ssrc,
                    endpointIdHash);
            }
        }

        {
            auto outboundContextItr = videoStream->_ssrcOutboundContexts.find(feedbackSsrc);
            if (outboundContextItr != videoStream->_ssrcOutboundContexts.end())
            {
                outboundContextItr->second._markedForDeletion = true;
                logger::info(
                    "Marking unused video outbound context for deletion, feedback ssrc %u, endpointIdHash %lu´",
                    _loggableId.c_str(),
                    ssrc,
                    endpointIdHash);
            }
        }
    }
}

void EngineMixer::markInboundContextForDeletion(const uint32_t ssrc)
{
    auto inboundContextItr = _ssrcInboundContexts.find(ssrc);
    if (inboundContextItr != _ssrcInboundContexts.end())
    {
        inboundContextItr->second._markedForDeletion = true;
        logger::info("Marking unused inbound context for deletion, ssrc %u", _loggableId.c_str(), ssrc);
    }
}

void EngineMixer::startRecordingAllCurrentStreams(EngineRecordingStream& recordingStream)
{
    if (recordingStream._isAudioEnabled)
    {
        updateRecordingAudioStreams(recordingStream, true);
    }

    if (recordingStream._isVideoEnabled)
    {
        updateRecordingVideoStreams(recordingStream, SimulcastStream::VideoContentType::VIDEO, true);
        _engineStreamDirector->addParticipant(recordingStream._endpointIdHash);
        sendDominantSpeakerToRecordingStream(recordingStream);
    }

    if (recordingStream._isScreenSharingEnabled)
    {
        updateRecordingVideoStreams(recordingStream, SimulcastStream::VideoContentType::SLIDES, true);
    }
}

void EngineMixer::sendRecordingAudioStream(EngineRecordingStream& targetStream,
    const EngineAudioStream& audioStream,
    bool isAdded)
{
    const auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);
    const auto ssrc = audioStream.remoteSsrc.isSet() ? audioStream.remoteSsrc.get() : 0;

    for (const auto& transportEntry : targetStream._transports)
    {
        auto unackedPacketsTrackerItr = targetStream._recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == targetStream._recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send recording audio stream event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        memory::UniquePacket packet;
        if (isAdded)
        {
            auto outboundContextIt = targetStream._ssrcOutboundContexts.find(ssrc);
            if (outboundContextIt != targetStream._ssrcOutboundContexts.end())
            {
                if (!outboundContextIt->second._markedForDeletion)
                {
                    // The event already was sent
                    // It happens when audio is reconfigured
                    // We will not send the event again
                    return;
                }

                outboundContextIt->second._markedForDeletion = false;
            }

            packet = recp::RecStreamAddedEventBuilder(_sendAllocator)
                         .setSequenceNumber(targetStream._recordingEventsOutboundContext._sequenceNumber++)
                         .setTimestamp(timestamp)
                         .setSsrc(ssrc)
                         .setRtpPayloadType(static_cast<uint8_t>(audioStream.rtpMap._payloadType))
                         .setPayloadFormat(audioStream.rtpMap._format)
                         .setEndpoint(audioStream.endpointId)
                         .setWallClock(std::chrono::system_clock::now())
                         .build();

            auto emplaceResult =
                targetStream._ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, audioStream.rtpMap);

            if (!emplaceResult.second && emplaceResult.first == targetStream._ssrcOutboundContexts.end())
            {
                logger::error("Failed to create outbound context for audio ssrc %u, rec transport %s",
                    _loggableId.c_str(),
                    ssrc,
                    transportEntry.second.getLoggableId().c_str());
            }
            else
            {
                logger::info("Created new outbound context for audio rec stream, rec endpointIdHash %lu, ssrc %u",
                    _loggableId.c_str(),
                    targetStream._endpointIdHash,
                    ssrc);
            }
        }
        else
        {
            packet = recp::RecStreamRemovedEventBuilder(_sendAllocator)
                         .setSequenceNumber(targetStream._recordingEventsOutboundContext._sequenceNumber++)
                         .setTimestamp(timestamp)
                         .setSsrc(ssrc)
                         .build();

            auto outboundContextItr = targetStream._ssrcOutboundContexts.find(ssrc);
            if (outboundContextItr != targetStream._ssrcOutboundContexts.end())
            {
                outboundContextItr->second._markedForDeletion = true;
            }
        }

        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as the we will not info about this stream
            logger::error("No space to allocate rec Stream%s event",
                _loggableId.c_str(),
                isAdded ? "Added" : "Removed");
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            targetStream._recordingEventsOutboundContext._packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::updateRecordingAudioStreams(EngineRecordingStream& targetStream, bool enabled)
{
    for (auto& audioStream : _engineAudioStreams)
    {
        sendRecordingAudioStream(targetStream, *audioStream.second, enabled);
    }
}

void EngineMixer::sendRecordingVideoStream(EngineRecordingStream& targetStream,
    const EngineVideoStream& videoStream,
    SimulcastStream::VideoContentType contentType,
    bool isAdded)
{
    if (videoStream._simulcastStream._contentType == contentType)
    {
        sendRecordingSimulcast(targetStream, videoStream, videoStream._simulcastStream, isAdded);
    }

    if (videoStream._secondarySimulcastStream.isSet() &&
        videoStream._secondarySimulcastStream.get()._contentType == contentType)
    {
        sendRecordingSimulcast(targetStream, videoStream, videoStream._secondarySimulcastStream.get(), isAdded);
    }
}

void EngineMixer::updateRecordingVideoStreams(EngineRecordingStream& targetStream,
    SimulcastStream::VideoContentType contentType,
    bool enabled)
{
    for (auto& videoStream : _engineVideoStreams)
    {
        sendRecordingVideoStream(targetStream, *videoStream.second, contentType, enabled);
    }
}

void EngineMixer::sendRecordingSimulcast(EngineRecordingStream& targetStream,
    const EngineVideoStream& videoStream,
    const SimulcastStream& simulcast,
    bool isAdded)
{
    // The ssrc will be rewritten using level 0
    const auto ssrc = simulcast._levels[0]._ssrc;
    if (ssrc == 0)
    {
        return;
    }

    for (const auto& transportEntry : targetStream._transports)
    {
        auto unackedPacketsTrackerItr = targetStream._recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == targetStream._recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send stream event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        memory::UniquePacket packet;
        if (isAdded)
        {
            auto outboundContextIt = targetStream._ssrcOutboundContexts.find(ssrc);
            if (outboundContextIt != targetStream._ssrcOutboundContexts.end())
            {
                if (!outboundContextIt->second._markedForDeletion)
                {
                    // The event already was sent
                    // It happens when audio is reconfigured
                    // We will not send the event again
                    return;
                }

                outboundContextIt->second._markedForDeletion = false;
            }

            packet = recp::RecStreamAddedEventBuilder(_sendAllocator)
                         .setSequenceNumber(targetStream._recordingEventsOutboundContext._sequenceNumber++)
                         .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL))
                         .setSsrc(ssrc)
                         .setIsScreenSharing(simulcast._contentType == SimulcastStream::VideoContentType::SLIDES)
                         .setRtpPayloadType(static_cast<uint8_t>(videoStream._rtpMap._payloadType))
                         .setPayloadFormat(videoStream._rtpMap._format)
                         .setEndpoint(videoStream._endpointId)
                         .setWallClock(std::chrono::system_clock::now())
                         .build();

            auto emplaceResult =
                targetStream._ssrcOutboundContexts.emplace(ssrc, ssrc, _sendAllocator, videoStream._rtpMap);

            if (!emplaceResult.second && emplaceResult.first == targetStream._ssrcOutboundContexts.end())
            {
                logger::error("Failed to create outbound context for video ssrc %u, rec transport %s",
                    _loggableId.c_str(),
                    ssrc,
                    transportEntry.second.getLoggableId().c_str());
            }
            else
            {
                logger::info("Created new outbound context for video rec stream, endpointIdHash %lu, ssrc %u",
                    _loggableId.c_str(),
                    targetStream._endpointIdHash,
                    ssrc);
            }
        }
        else
        {
            packet = recp::RecStreamRemovedEventBuilder(_sendAllocator)
                         .setSequenceNumber(targetStream._recordingEventsOutboundContext._sequenceNumber++)
                         .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL))
                         .setSsrc(ssrc)
                         .build();

            auto outboundContextItr = targetStream._ssrcOutboundContexts.find(ssrc);
            if (outboundContextItr != targetStream._ssrcOutboundContexts.end())
            {
                outboundContextItr->second._markedForDeletion = true;
            }
        }

        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as the we will not info about this stream
            logger::error("No space to allocate rec Stream%s event",
                _loggableId.c_str(),
                isAdded ? "Added" : "Removed");
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            targetStream._recordingEventsOutboundContext._packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::sendAudioStreamToRecording(const EngineAudioStream& audioStream, bool isAdded)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->_isAudioEnabled)
        {
            sendRecordingAudioStream(*rec.second, audioStream, isAdded);
        }
    }
}

void EngineMixer::sendVideoStreamToRecording(const EngineVideoStream& videoStream, bool isAdded)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->_isVideoEnabled)
        {
            sendRecordingVideoStream(*rec.second, videoStream, SimulcastStream::VideoContentType::VIDEO, isAdded);
        }

        if (rec.second->_isScreenSharingEnabled)
        {
            sendRecordingVideoStream(*rec.second, videoStream, SimulcastStream::VideoContentType::SLIDES, isAdded);
        }
    }
}

void EngineMixer::removeVideoSsrcFromRecording(const EngineVideoStream& videoStream, uint32_t ssrc)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->_isVideoEnabled)
        {
            if (videoStream._simulcastStream._levels[0]._ssrc == ssrc &&
                videoStream._simulcastStream._contentType == SimulcastStream::VideoContentType::VIDEO)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream._simulcastStream, false);
            }

            if (videoStream._secondarySimulcastStream.isSet() &&
                videoStream._secondarySimulcastStream.get()._levels[0]._ssrc == ssrc &&
                videoStream._secondarySimulcastStream.get()._contentType == SimulcastStream::VideoContentType::VIDEO)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream._simulcastStream, false);
            }
        }

        if (rec.second->_isScreenSharingEnabled)
        {
            if (videoStream._simulcastStream._levels[0]._ssrc == ssrc &&
                videoStream._simulcastStream._contentType == SimulcastStream::VideoContentType::SLIDES)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream._simulcastStream, false);
            }

            if (videoStream._secondarySimulcastStream.isSet() &&
                videoStream._secondarySimulcastStream.get()._levels[0]._ssrc == ssrc &&
                videoStream._secondarySimulcastStream.get()._contentType == SimulcastStream::VideoContentType::SLIDES)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream._secondarySimulcastStream.get(), false);
            }
        }
    }
}

void EngineMixer::allocateRecordingRtpPacketCacheIfNecessary(SsrcOutboundContext& ssrcOutboundContext,
    EngineRecordingStream& recordingStream)
{
    if (!ssrcOutboundContext._packetCache.isSet())
    {
        ssrcOutboundContext._packetCache.set(nullptr);
        EngineMessage::Message message(EngineMessage::Type::AllocateRecordingRtpPacketCache);
        message.command.allocateRecordingRtpPacketCache.mixer = this;
        message.command.allocateRecordingRtpPacketCache.ssrc = ssrcOutboundContext._ssrc;
        message.command.allocateRecordingRtpPacketCache.endpointIdHash = recordingStream._endpointIdHash;
        _messageListener.onMessage(std::move(message));
    }
}

void EngineMixer::processRecordingMissingPackets(const uint64_t timestamp)
{
    for (auto& engineRecordingStreamEntry : _engineRecordingStreams)
    {
        auto engineRecordingStream = engineRecordingStreamEntry.second;
        for (auto& recEventMissingPacketsTrackerEntry : engineRecordingStream->_recEventUnackedPacketsTracker)
        {
            auto& recEventMissingPacketsTracker = recEventMissingPacketsTrackerEntry.second;
            if (!recEventMissingPacketsTracker.shouldProcess(timestamp / 1000000ULL))
            {
                continue;
            }

            auto transportItr = engineRecordingStream->_transports.find(recEventMissingPacketsTrackerEntry.first);
            if (transportItr == engineRecordingStream->_transports.end())
            {
                continue;
            }

            auto& transport = transportItr->second;
            transport.getJobQueue().addJob<ProcessUnackedRecordingEventPacketsJob>(
                engineRecordingStream->_recordingEventsOutboundContext,
                recEventMissingPacketsTracker,
                transport,
                _sendAllocator);
        }
    }
}

bool EngineMixer::needToUpdateMinUplinkEstimate(const uint32_t curEstimate, const uint32_t oldEstimate) const
{
    // For screensharing (a.k.a. 'slides') we have a minumum allowed bitrate of 900 kbps,
    // so it does not worth to react on the fluctuation below ~10% of this value.
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

void EngineMixer::reportMinRemoteClientDownlinkBandwidthToBarbells(const uint32_t minUplinkEstimate) const
{
    utils::StringBuilder<1024> message;
    api::DataChannelMessage::makeMinUplinkBitrate(message, minUplinkEstimate);
    for (const auto& itBb : _engineBarbells)
    {
        itBb.second->dataChannel.sendData(message.get(), message.getLength());
    }
}

void EngineMixer::checkVideoBandwidth(const uint64_t timestamp)
{
    if (!utils::Time::diffGE(_lastVideoBandwidthCheck, timestamp, utils::Time::sec * 3))
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
        if (videoStream._simulcastStream._contentType == SimulcastStream::VideoContentType::SLIDES)
        {
            presenterSimulcastLevel = &videoStream._simulcastStream._levels[0];
            presenterStream = videoIt.second;
        }
        else if (videoStream._secondarySimulcastStream.isSet() &&
            videoStream._secondarySimulcastStream.get()._contentType == SimulcastStream::VideoContentType::SLIDES)
        {
            presenterSimulcastLevel = &videoStream._secondarySimulcastStream.get()._levels[0];
            presenterStream = videoIt.second;
        }
        else
        {
            minUplinkEstimate = std::min(minUplinkEstimate, videoIt.second->_transport.getUplinkEstimateKbps());
        }
    }

    minUplinkEstimate = std::max(minUplinkEstimate, _config.slides.minBitrate.get());
    if (needToUpdateMinUplinkEstimate(minUplinkEstimate, _minUplinkEstimate))
    {
        reportMinRemoteClientDownlinkBandwidthToBarbells(minUplinkEstimate);
    }

    minUplinkEstimate =
        std::max(_config.slides.minBitrate.get(), std::min(minUplinkEstimate, getMinRemoteClientDownlinkBandwidth()));

    if (presenterSimulcastLevel)
    {
        const uint32_t slidesLimit = minUplinkEstimate * _config.slides.allocFactor;

        logger::info("limiting bitrate for ssrc %u, at %u",
            _loggableId.c_str(),
            presenterSimulcastLevel->_ssrc,
            slidesLimit);

        presenterStream->_transport.getJobQueue().addJob<SetMaxMediaBitrateJob>(presenterStream->_transport,
            presenterStream->_localSsrc,
            presenterSimulcastLevel->_ssrc,
            slidesLimit,
            _sendAllocator);
    }
}

void EngineMixer::startProbingVideoStream(EngineVideoStream& engineVideoStream)
{
    auto* outboundContext = obtainOutboundSsrcContext(engineVideoStream._endpointIdHash,
        engineVideoStream._ssrcOutboundContexts,
        engineVideoStream._localSsrc,
        engineVideoStream._feedbackRtpMap);

    if (outboundContext)
    {
        engineVideoStream._transport.getJobQueue().addJob<SetRtxProbeSourceJob>(engineVideoStream._transport,
            engineVideoStream._localSsrc,
            &outboundContext->_sequenceCounter,
            outboundContext->_rtpMap._payloadType);
    }
    _probingVideoStreams = true;
}

void EngineMixer::stopProbingVideoStream(const EngineVideoStream& engineVideoStream)
{
    engineVideoStream._transport.getJobQueue().addJob<SetRtxProbeSourceJob>(engineVideoStream._transport,
        engineVideoStream._localSsrc,
        nullptr,
        engineVideoStream._feedbackRtpMap._payloadType);
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

void EngineMixer::runTransportTicks(const uint64_t timestamp)
{
    for (auto videoIt : _engineVideoStreams)
    {
        auto& videoStream = *videoIt.second;
        videoStream._transport.runTick(timestamp);
    }
}

void EngineMixer::addBarbell(EngineBarbell* barbell)
{
    const auto idHash = barbell->transport.getEndpointIdHash();
    if (_engineBarbells.find(idHash) != _engineBarbells.end())
    {
        return;
    }

    logger::debug("Add engine barbell, transport %s, idHash %lu",
        _loggableId.c_str(),
        barbell->transport.getLoggableId().c_str(),
        idHash);

    _engineBarbells.emplace(idHash, barbell);
}

void EngineMixer::removeBarbell(size_t idHash)
{
    if (_engineBarbells.find(idHash) != _engineBarbells.end())
    {
        return;
    }

    _engineBarbells.erase(idHash);
}

size_t EngineMixer::getDominantSpeakerId() const
{
    return _activeMediaList->getDominantSpeaker();
}

std::map<size_t, ActiveTalker> EngineMixer::getActiveTalkers() const
{
    return _activeMediaList->getActiveTalkers();
}

utils::Optional<uint32_t> EngineMixer::getUserId(const size_t ssrc) const
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

namespace
{
template <typename TArray>
void copyToBarbellMapItemArray(utils::SimpleJsonArray& endpointArray, TArray& target)
{
    for (auto endpoint : endpointArray)
    {
        BarbellMapItem item;

        endpoint["endpoint-id"].getString(item.endpointId);
        item.endpointIdHash = utils::hash<char*>{}(item.endpointId);
        for (auto ssrc : endpoint["ssrcs"].getArray())
        {
            item.ssrcs.push_back(ssrc.getInt<uint32_t>(0));
        }

        target.push_back(item);
    }
}
} // namespace
// This method must be executed on engine thread. UMM requests could go in another queue and processed
// at start of tick.
// There are three phases to update the endpoints and ssrc mappings in active media list
// phase 1: Identify all endpoints that are registered but no longer in UMM, and remove them from AML
// phase 2: Identify all streams that have been remapped to other ssrcs, and remove them from AML
// phase 3: All ssrcs in UMM that lacks existing mapping in EngineBarbell, must be new or changed. Add them to AML
void EngineMixer::onBarbellUserMediaMap(size_t barbellIdHash, const char* message)
{
    auto it = _engineBarbells.find(barbellIdHash);
    if (it == _engineBarbells.end())
    {
        return;
    }

    auto mediaMapJson = utils::SimpleJson::create(message, strlen(message));

    EngineBarbell& barbell = *(it->second);

    logger::info("received BB msg over barbell %s %zu, json %s",
        _loggableId.c_str(),
        barbell.id.c_str(),
        barbellIdHash,
        message);

    auto videoEndpointsArray = mediaMapJson["video-endpoints"].getArray();
    auto audioEnpointsArray = mediaMapJson["audio-endpoints"].getArray();

    memory::Array<BarbellMapItem, 12> videoSsrcs;
    memory::Array<BarbellMapItem, 8> audioSsrcs;

    copyToBarbellMapItemArray(videoEndpointsArray, videoSsrcs);
    copyToBarbellMapItemArray(audioEnpointsArray, audioSsrcs);

    memory::Map<size_t, size_t, 32> fwdVideoEndpoints;
    for (auto& item : videoSsrcs)
    {
        fwdVideoEndpoints.add(item.endpointIdHash, item.endpointIdHash);
    }

    memory::Map<size_t, size_t, 32> fwdAudioEndpoints;
    for (auto& item : audioSsrcs)
    {
        fwdAudioEndpoints.add(item.endpointIdHash, item.endpointIdHash);
    }

    const auto mapRevision = _activeMediaList->getMapRevision();

    // clear out removed streams
    for (auto& videoStream : barbell.videoStreams)
    {
        if (videoStream.endpointIdHash.isSet() && !fwdVideoEndpoints.contains(videoStream.endpointIdHash.get()))
        {
            _activeMediaList->removeVideoParticipant(videoStream.endpointIdHash.get());
            _engineStreamDirector->removeParticipant(videoStream.endpointIdHash.get());
            videoStream.endpointIdHash.clear();
            videoStream.endpointId.clear();
        }
    }

    for (auto& audioStream : barbell.audioStreams)
    {
        if (audioStream.endpointIdHash.isSet() && !fwdAudioEndpoints.contains(audioStream.endpointIdHash.get()))
        {
            _activeMediaList->removeAudioParticipant(audioStream.endpointIdHash.get());
            audioStream.endpointIdHash.clear();
            audioStream.endpointId.clear();
        }
    }

    // remove remapped ones
    for (auto& item : videoSsrcs)
    {
        for (size_t i = 0; i < item.ssrcs.size(); ++i)
        {
            const auto ssrc = item.ssrcs[i];
            auto* videoStream = barbell.videoSsrcMap.getItem(ssrc);
            if (!videoStream)
            {
                logger::error("video ssrc in barbell UMM does not exist %u", _loggableId.c_str(), ssrc);
            }
            else if (videoStream->endpointIdHash.isSet() && videoStream->endpointIdHash.get() != item.endpointIdHash)
            {
                // must remove as this ssrc has been remapped
                _activeMediaList->removeVideoParticipant(videoStream->endpointIdHash.get());
                _engineStreamDirector->removeParticipant(videoStream->endpointIdHash.get());
                videoStream->endpointIdHash.clear();
                videoStream->endpointId.clear();
            }
        }
    }

    for (auto& item : audioSsrcs)
    {
        for (size_t i = 0; i < item.ssrcs.size(); ++i)
        {
            const auto ssrc = item.ssrcs[i];
            auto* audioStream = barbell.audioSsrcMap.getItem(ssrc);
            if (!audioStream)
            {
                logger::error("audio ssrc in barbell UMM does not exist %u", _loggableId.c_str(), ssrc);
            }
            else if (audioStream->endpointIdHash.isSet() && audioStream->endpointIdHash.get() != item.endpointIdHash)
            {
                // must remove as this ssrc has been remapped
                _activeMediaList->removeAudioParticipant(audioStream->endpointIdHash.get());
                audioStream->endpointIdHash.clear();
                audioStream->endpointId.clear();
            }
        }
    }

    // add video sources
    for (auto& item : videoSsrcs)
    {
        if (item.ssrcs.size() == 0)
        {
            continue;
        }

        auto* videoStream = barbell.videoSsrcMap.getItem(item.ssrcs[0]);
        if (!videoStream || videoStream->endpointIdHash.isSet())
        {
            // if it is set it must already be set to this endpointId
            continue;
        }
        videoStream->endpointIdHash.set(item.endpointIdHash);
        videoStream->endpointId.set(item.endpointId);
        SimulcastStream stream0 = videoStream->stream;

        utils::Optional<SimulcastStream> stream1;
        if (item.ssrcs.size() > 1)
        {
            auto* videoStream = barbell.videoSsrcMap.getItem(item.ssrcs[1]);
            if (videoStream && videoStream->endpointIdHash.isSet())
            {
                videoStream->endpointIdHash.set(item.endpointIdHash);
                videoStream->endpointId.set(item.endpointId);

                stream1.set(videoStream->stream);
            }
        }

        _activeMediaList->addBarbellVideoParticipant(item.endpointIdHash, stream0, stream1, item.endpointId);
        _engineStreamDirector->addParticipant(item.endpointIdHash, stream0, stream1.isSet() ? &stream1.get() : nullptr);
    }

    // add audio sources
    for (auto& item : audioSsrcs)
    {
        if (item.ssrcs.size() == 0)
        {
            continue;
        }

        auto* audioStream = barbell.audioSsrcMap.getItem(item.ssrcs[0]);
        if (!audioStream || audioStream->endpointIdHash.isSet())
        {
            // if it is set it must already be set to this endpointId
            continue;
        }

        audioStream->endpointIdHash.set(item.endpointIdHash);
        audioStream->endpointId.set(item.endpointId);
        _activeMediaList->addBarbellAudioParticipant(item.endpointIdHash, item.endpointId);
    }

    if (mapRevision != _activeMediaList->getMapRevision())
    {
        // only update local users
        sendUserMediaMapMessageToAll();
    }
}

void EngineMixer::onBarbellMinUplinkEstimate(size_t barbellIdHash, const char* message)
{
    auto it = _engineBarbells.find(barbellIdHash);
    if (it == _engineBarbells.end())
    {
        return;
    }

    EngineBarbell& barbell = *(it->second);

    logger::debug("received BB msg over barbell %s %zu, json %s",
        _loggableId.c_str(),
        barbell.id.c_str(),
        barbellIdHash,
        message);

    barbell.minClientDownlinkBandwidth =
        api::DataChannelMessageParser::getMinUplinkBirate(utils::SimpleJson::create(message, strlen(message)));
}
} // namespace bridge
