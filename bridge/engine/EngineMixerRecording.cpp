#include "bridge/MixerManagerAsync.h"
#include "bridge/RecordingDescription.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/AddPacketCacheJob.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineStreamDirector.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/ProcessUnackedRecordingEventPacketsJob.h"
#include "bridge/engine/RecordingAudioForwarderSendJob.h"
#include "bridge/engine/RecordingEventAckReceiveJob.h"
#include "bridge/engine/RecordingRtpNackReceiveJob.h"
#include "bridge/engine/RecordingSendEventJob.h"
#include "bridge/engine/RecordingVideoForwarderSendJob.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecControlHeader.h"
#include "transport/recp/RecDominantSpeakerEventBuilder.h"
#include "transport/recp/RecStartStopEventBuilder.h"
#include "transport/recp/RecStreamAddedEventBuilder.h"
#include "transport/recp/RecStreamRemovedEventBuilder.h"

namespace bridge
{
const uint64_t RECUNACK_PROCESS_INTERVAL = 25 * utils::Time::ms;

class FinalizeRecordingOutboundContextJob : public jobmanager::CountedJob
{
public:
    FinalizeRecordingOutboundContextJob(bridge::EngineMixer& mixer,
        transport::RecordingTransport& transport,
        size_t recordingStreamIdHash,
        concurrency::SynchronizationContext& engineSyncContext,
        uint32_t ssrc,
        std::atomic_uint32_t& finalizerCounter)
        : CountedJob(transport.getJobCounter()),
          _mixer(mixer),
          _engineSyncContext(engineSyncContext),
          _recordingStreamIdHash(recordingStreamIdHash),
          _ssrc(ssrc),
          _finalizerCounter(finalizerCounter)
    {
        ++finalizerCounter;
    }

    void run() override
    {
        if (0 == --_finalizerCounter)
        {
            _engineSyncContext.post(
                [&]() { _mixer.onRecordingOutboundContextFinalized(_recordingStreamIdHash, _ssrc); });
        }
    }

private:
    bridge::EngineMixer& _mixer;
    concurrency::SynchronizationContext& _engineSyncContext;
    size_t _recordingStreamIdHash;
    uint32_t _ssrc;
    std::atomic_uint32_t& _finalizerCounter;
};

void EngineMixer::addRecordingStream(EngineRecordingStream* engineRecordingStream)
{
    if (_engineRecordingStreams.contains(engineRecordingStream->endpointIdHash))
    {
        assert(false);
        return;
    }

    _engineRecordingStreams.emplace(engineRecordingStream->endpointIdHash, engineRecordingStream);
}

void EngineMixer::removeRecordingStream(const EngineRecordingStream& engineRecordingStream)
{
    logger::debug("Remove recordingStream", _loggableId.c_str());
    _engineStreamDirector->removeParticipant(engineRecordingStream.endpointIdHash);
    _engineStreamDirector->removeParticipantPins(engineRecordingStream.endpointIdHash);
    _engineRecordingStreams.erase(engineRecordingStream.endpointIdHash);
}

void EngineMixer::updateRecordingStreamModalities(EngineRecordingStream& engineRecordingStream,
    bool isAudioEnabled,
    bool isVideoEnabled,
    bool isScreenSharingEnabled)
{
    if (!engineRecordingStream.isReady)
    {
        // I think this is very unlikely or even impossible to happen
        logger::warn("Received a stream update modality but the stream is not ready yet. endpointIdHash %lu",
            _loggableId.c_str(),
            engineRecordingStream.endpointIdHash);
        return;
    }

    logger::debug("Update recordingStream modalities, stream: %s audio: %s, video: %s",
        getLoggableId().c_str(),
        engineRecordingStream.id.c_str(),
        isAudioEnabled ? "enabled" : "disabled",
        isVideoEnabled ? "enabled" : "disabled");

    if (engineRecordingStream.isAudioEnabled != isAudioEnabled)
    {
        engineRecordingStream.isAudioEnabled = isAudioEnabled;
        updateRecordingAudioStreams(engineRecordingStream, isAudioEnabled);
    }

    if (engineRecordingStream.isVideoEnabled != isVideoEnabled)
    {
        engineRecordingStream.isVideoEnabled = isVideoEnabled;
        updateRecordingVideoStreams(engineRecordingStream, SimulcastStream::VideoContentType::VIDEO, isVideoEnabled);
    }

    if (engineRecordingStream.isScreenSharingEnabled != isScreenSharingEnabled)
    {
        engineRecordingStream.isScreenSharingEnabled = isScreenSharingEnabled;
        updateRecordingVideoStreams(engineRecordingStream,
            SimulcastStream::VideoContentType::SLIDES,
            isScreenSharingEnabled);
    }
}

void EngineMixer::startRecordingTransport(transport::RecordingTransport& transport)
{
    logger::debug("Starting recording transport %s", transport.getLoggableId().c_str(), _loggableId.c_str());
    transport.setDataReceiver(this);

    transport.start();
    transport.connect();
}

void EngineMixer::recordingStart(EngineRecordingStream& stream, const RecordingDescription& desc)
{
    auto seq = stream.recordingEventsOutboundContext.sequenceNumber++;
    auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : stream.transports)
    {
        auto unackedPacketsTrackerItr = stream.recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == stream.recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send start recording event to transport %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            continue;
        }

        auto packet = recp::RecStartStopEventBuilder(_sendAllocator)
                          .setAudioEnabled(desc.isAudioEnabled)
                          .setVideoEnabled(desc.isVideoEnabled)
                          .setSequenceNumber(seq)
                          .setTimestamp(timestamp)
                          .setRecordingId(desc.recordingId)
                          .setUserId(desc.ownerId)
                          .build();
        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail. We have to find a way to report this glitch
            logger::error("No space available to allocate rec start event", _loggableId.c_str());
            continue;
        }

        logger::info("Sent recording start event for recordingId %s, streamIdHash %zu, transport %zu",
            _loggableId.c_str(),
            desc.recordingId.c_str(),
            stream.endpointIdHash,
            transportEntry.first);

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            stream.recordingEventsOutboundContext.packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::stopRecording(EngineRecordingStream& stream, const RecordingDescription& desc)
{
    const auto sequenceNumber = stream.recordingEventsOutboundContext.sequenceNumber++;
    const auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : stream.transports)
    {
        auto unackedPacketsTrackerItr = stream.recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == stream.recEventUnackedPacketsTracker.end())
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
                          .setRecordingId(desc.recordingId)
                          .setUserId(desc.ownerId)
                          .build();

        if (!packet)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as we will not know when it finish. We have to find a way to report this glitch
            logger::error("No space available to allocate rec stop event", _loggableId.c_str());
            continue;
        }

        logger::info("Sent recording stop event for recordingId %s, streamIdHash %zu, transport %zu",
            _loggableId.c_str(),
            desc.recordingId.c_str(),
            stream.endpointIdHash,
            transportEntry.first);

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packet),
            transportEntry.second,
            stream.recordingEventsOutboundContext.packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::addRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* packetCache)
{
    assert(endpointIdHash);

    auto recordingStream = _engineRecordingStreams.getItem(endpointIdHash);
    if (!recordingStream)
    {
        return;
    }

    auto* outboundContext = recordingStream->ssrcOutboundContexts.getItem(ssrc);
    if (outboundContext)
    {
        if (outboundContext->markedForDeletion)
        {
            logger::warn("Outbound context for recording video packet cache is marked for deletion. ssrc %u",
                _loggableId.c_str(),
                ssrc);
            return;
            return;
        }

        auto* transport = recordingStream->transports.getItem(endpointIdHash);
        if (!transport)
        {
            logger::warn("cannot find transport and outbound context for recording video packet cache. ssrc %u",
                _loggableId.c_str(),
                ssrc);
            return;
        }

        transport->getJobQueue().addJob<AddPacketCacheJob>(*transport, *outboundContext, packetCache);
    }
}

void EngineMixer::addTransportToRecordingStream(const size_t streamIdHash,
    transport::RecordingTransport& transport,
    UnackedPacketsTracker& recUnackedPacketsTracker)
{
    auto* recordingStream = _engineRecordingStreams.getItem(streamIdHash);
    if (!recordingStream)
    {
        return;
    }

    recordingStream->transports.emplace(transport.getEndpointIdHash(), transport);
    recordingStream->recEventUnackedPacketsTracker.emplace(transport.getEndpointIdHash(), recUnackedPacketsTracker);

    if (!recordingStream->isReady)
    {
        recordingStream->isReady = true;
        startRecordingAllCurrentStreams(*recordingStream);
    }
}

void EngineMixer::removeTransportFromRecordingStream(const size_t streamIdHash, const size_t endpointIdHash)
{
    auto* recordingStream = _engineRecordingStreams.getItem(streamIdHash);
    if (!recordingStream)
    {
        return;
    }

    recordingStream->transports.erase(endpointIdHash);
    recordingStream->recEventUnackedPacketsTracker.erase(endpointIdHash);

    _messageListener.asyncRemoveRecordingTransport(*this, recordingStream->id.c_str(), endpointIdHash);
}

void EngineMixer::onRecControlReceived(transport::RecordingTransport* sender,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    auto* recordingStream = _engineRecordingStreams.getItem(sender->getStreamIdHash());
    if (!recordingStream)
    {
        return;
    }

    auto recControlHeader = recp::RecControlHeader::fromPacket(*packet);
    if (recControlHeader->isEventAck())
    {
        auto* unackedPacketsTracker =
            recordingStream->recEventUnackedPacketsTracker.getItem(sender->getEndpointIdHash());
        if (!unackedPacketsTracker)
        {
            return;
        }

        sender->getJobQueue().addJob<RecordingEventAckReceiveJob>(std::move(packet), sender, *unackedPacketsTracker);
    }
    else if (recControlHeader->isRtpNack())
    {
        auto ssrc = recControlHeader->getSsrc();
        auto ssrcContext = recordingStream->ssrcOutboundContexts.getItem(ssrc);
        if (!ssrcContext || ssrcContext->markedForDeletion)
        {
            return;
        }

        sender->getJobQueue().addJob<RecordingRtpNackReceiveJob>(std::move(packet),
            _sendAllocator,
            sender,
            *ssrcContext);
    }
}

void EngineMixer::onRecordingOutboundContextFinalized(size_t recordingStreamIdHash, uint32_t ssrc)
{
    auto* recordingStream = _engineRecordingStreams.getItem(recordingStreamIdHash);
    if (!recordingStream)
    {
        return;
    }

    auto& outboundContexts = recordingStream->ssrcOutboundContexts;
    const auto outboundContextIt = outboundContexts.find(ssrc);
    if (outboundContextIt == outboundContexts.end())
    {
        return;
    }

    // It can happen that outbound context can be re-activated after marked for deletion. Then we will check if it
    // is still marked for deletion.
    if (!outboundContextIt->second.markedForDeletion)
    {
        logger::info("Recording outbound context was re-activated for endpointId %zu, ssrc %u",
            _loggableId.c_str(),
            recordingStreamIdHash,
            ssrc);

        recordingStream->outboundContextsFinalizerCounter.erase(ssrc);
        return;
    }

    auto& outboundContextsFinalizerCounter = recordingStream->outboundContextsFinalizerCounter;
    auto finalizeCounterIt = outboundContextsFinalizerCounter.find(ssrc);
    assert(finalizeCounterIt != outboundContextsFinalizerCounter.end());
    if (finalizeCounterIt != outboundContextsFinalizerCounter.end())
    {
        // It's allowed to re-active outbound ssrc, even though this should never happen.
        // but for safety we will verify if a reactivation has provoked new finalized jobs
        if (finalizeCounterIt->second != 0)
        {
            logger::warn("Finalize counter is not zero yet for endpoint %zu, ssrc %u",
                _loggableId.c_str(),
                recordingStreamIdHash,
                ssrc);
            return;
        }

        outboundContextsFinalizerCounter.erase(ssrc);
    }

    logger::info("Removing recording outbound context for endpoint %zu, ssrc %u",
        _loggableId.c_str(),
        recordingStreamIdHash,
        ssrc);

    _messageListener.asyncFreeRecordingRtpPacketCache(*this, outboundContextIt->second.ssrc, recordingStreamIdHash);
    outboundContexts.erase(outboundContextIt->first);
}

void EngineMixer::forwardAudioRtpPacketRecording(IncomingPacketInfo& packetInfo, uint64_t timestamp)
{
    if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()) || !packetInfo.inboundContext())
    {
        return;
    }

    for (auto& recordingStreams : _engineRecordingStreams)
    {
        auto* recordingStream = recordingStreams.second;
        if (!(recordingStream && recordingStream->isAudioEnabled))
        {
            continue;
        }

        const bridge::SsrcInboundContext* senderInboundContext = packetInfo.inboundContext();
        const auto ssrc = senderInboundContext->ssrc;
        auto* ssrcOutboundContext = recordingStream->ssrcOutboundContexts.getItem(ssrc);
        if (!ssrcOutboundContext || ssrcOutboundContext->recordingOutboundDecommissioned)
        {
            continue;
        }

        for (const auto& transportEntry : recordingStream->transports)
        {
            ssrcOutboundContext->onRtpSent(timestamp);
            auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
            if (packet)
            {
                transportEntry.second.getJobQueue().addJob<RecordingAudioForwarderSendJob>(*ssrcOutboundContext,
                    std::move(packet),
                    transportEntry.second,
                    *senderInboundContext,
                    packetInfo.extendedSequenceNumber(),
                    _messageListener,
                    transportEntry.first,
                    *this,
                    timestamp);
            }
            else
            {
                logger::warn("send allocator depleted RecFwdSend", _loggableId.c_str());
            }
        }
    }
}

void EngineMixer::forwardVideoRtpPacketRecording(IncomingPacketInfo& packetInfo, const uint64_t timestamp)
{
    if (EngineBarbell::isFromBarbell(packetInfo.transport()->getTag()) || !packetInfo.inboundContext())
    {
        return;
    }

    for (auto& recordingStreams : _engineRecordingStreams)
    {
        auto* recordingStream = recordingStreams.second;
        if (!(recordingStream && (recordingStream->isVideoEnabled || recordingStream->isScreenSharingEnabled)))
        {
            continue;
        }

        if (!_engineStreamDirector->shouldRecordSsrc(recordingStream->endpointIdHash,
                packetInfo.inboundContext()->ssrc))
        {
            continue;
        }

        auto* ssrcOutboundContext =
            recordingStream->ssrcOutboundContexts.getItem(packetInfo.inboundContext()->defaultLevelSsrc);
        if (!ssrcOutboundContext || ssrcOutboundContext->recordingOutboundDecommissioned)
        {
            continue;
        }

        for (const auto& transportEntry : recordingStream->transports)
        {
            ssrcOutboundContext->onRtpSent(timestamp); // active jobs on this ssrc context
            auto packet = memory::makeUniquePacket(_sendAllocator, *packetInfo.packet());
            if (packet)
            {
                transportEntry.second.getJobQueue().addJob<RecordingVideoForwarderSendJob>(*ssrcOutboundContext,
                    *(packetInfo.inboundContext()),
                    std::move(packet),
                    transportEntry.second,
                    packetInfo.extendedSequenceNumber(),
                    _messageListener,
                    transportEntry.first,
                    *this,
                    timestamp);
            }
            else
            {
                logger::warn("send allocator depleted FwdRewrite", _loggableId.c_str());
            }
        }
    }
}

void EngineMixer::sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream,
    const size_t dominantSpeaker,
    const std::string& dominantSpeakerEndpoint)
{
    if (recordingStream.isVideoEnabled)
    {
        pinEndpoint(recordingStream.endpointIdHash, dominantSpeaker);
    }

    const auto sequenceNumber = recordingStream.recordingEventsOutboundContext.sequenceNumber++;
    const auto timestamp = static_cast<uint32_t>(utils::Time::getAbsoluteTime() / 1000000ULL);

    for (const auto& transportEntry : recordingStream.transports)
    {
        auto unackedPacketsTrackerItr = recordingStream.recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == recordingStream.recEventUnackedPacketsTracker.end())
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
            recordingStream.recordingEventsOutboundContext.packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream)
{
    const auto dominantSpeaker = _activeMediaList->getDominantSpeaker();
    auto* audioStream = _engineAudioStreams.getItem(dominantSpeaker);
    if (audioStream)
    {
        sendDominantSpeakerToRecordingStream(recordingStream, dominantSpeaker, audioStream->endpointId);
    }
}

void EngineMixer::startRecordingAllCurrentStreams(EngineRecordingStream& recordingStream)
{
    if (recordingStream.isAudioEnabled)
    {
        updateRecordingAudioStreams(recordingStream, true);
    }

    if (recordingStream.isVideoEnabled)
    {
        updateRecordingVideoStreams(recordingStream, SimulcastStream::VideoContentType::VIDEO, true);
        _engineStreamDirector->addParticipant(recordingStream.endpointIdHash);
        sendDominantSpeakerToRecordingStream(recordingStream);
    }

    if (recordingStream.isScreenSharingEnabled)
    {
        updateRecordingVideoStreams(recordingStream, SimulcastStream::VideoContentType::SLIDES, true);
    }
}

void EngineMixer::sendRecordingAudioStream(EngineRecordingStream& targetStream,
    const EngineAudioStream& audioStream,
    bool isAdded)
{
    const auto ssrc = audioStream.remoteSsrc.valueOr(0);
    if (ssrc == 0)
    {
        return;
    }

    if (targetStream.transports.empty())
    {
        logger::error("Audio recording stream has zero transports. endpointHashId %zu",
            _loggableId.c_str(),
            targetStream.endpointIdHash);

        return;
    }

    memory::UniquePacket packet;
    auto outboundContextIt = targetStream.ssrcOutboundContexts.find(ssrc);
    if (isAdded)
    {
        if (outboundContextIt != targetStream.ssrcOutboundContexts.end())
        {
            if (!outboundContextIt->second.recordingOutboundDecommissioned)
            {
                // The event already was sent
                // It happens when audio is reconfigured
                // We will not send the event again
                return;
            }

            outboundContextIt->second.recordingOutboundDecommissioned = false;
            outboundContextIt->second.markedForDeletion = false;

            logger::info("Reactivating decommissioned recording outbound video ssrc %u for recording stream %zu",
                _loggableId.c_str(),
                ssrc,
                targetStream.endpointIdHash);
        }
        else
        {
            auto emplaceResult = targetStream.ssrcOutboundContexts.emplace(ssrc,
                ssrc,
                _sendAllocator,
                audioStream.rtpMap,
                audioStream.telephoneEventRtpMap);

            if (!emplaceResult.second && emplaceResult.first == targetStream.ssrcOutboundContexts.end())
            {
                logger::error("Failed to create outbound context for audio ssrc %u, recording stream %zu",
                    _loggableId.c_str(),
                    ssrc,
                    targetStream.endpointIdHash);
            }
            else
            {
                logger::info("Created new outbound context for audio recording stream  %zu, ssrc %u",
                    _loggableId.c_str(),
                    targetStream.endpointIdHash,
                    ssrc);
            }
        }

        packet = recp::RecStreamAddedEventBuilder(_sendAllocator)
                     .setSequenceNumber(targetStream.recordingEventsOutboundContext.sequenceNumber++)
                     .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / utils::Time::ms))
                     .setSsrc(ssrc)
                     .setRtpPayloadType(static_cast<uint8_t>(audioStream.rtpMap.payloadType))
                     .setPayloadFormat(audioStream.rtpMap.format)
                     .setEndpoint(audioStream.endpointId)
                     .setWallClock(utils::Time::now())
                     .build();

        logger::info("Audio ssrc %u added to recording %zu", _loggableId.c_str(), ssrc, targetStream.endpointIdHash);
    }
    else
    {
        if (outboundContextIt != targetStream.ssrcOutboundContexts.end())
        {
            outboundContextIt->second.recordingOutboundDecommissioned = true;
        }

        packet = recp::RecStreamRemovedEventBuilder(_sendAllocator)
                     .setSequenceNumber(targetStream.recordingEventsOutboundContext.sequenceNumber++)
                     .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / utils::Time::ms))
                     .setSsrc(ssrc)
                     .build();

        logger::info("Audio ssrc %u removed from recording %zu",
            _loggableId.c_str(),
            ssrc,
            targetStream.endpointIdHash);
    }

    auto lastKey = (--targetStream.transports.end())->first;
    for (const auto& transportEntry : targetStream.transports)
    {
        auto unackedPacketsTrackerItr = targetStream.recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == targetStream.recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send recording audio stream event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            return;
        }

        memory::UniquePacket packetToSend;
        if (lastKey == transportEntry.first)
        {
            packetToSend = std::move(packet);
        }
        else
        {
            packetToSend = makeUniquePacket(_sendAllocator, *packet);
        }

        if (!packetToSend)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as the we will not info about this stream
            logger::error("No space to allocate rec AudioStream%s event",
                _loggableId.c_str(),
                isAdded ? "Added" : "Removed");
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packetToSend),
            transportEntry.second,
            targetStream.recordingEventsOutboundContext.packetCache,
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
    if (videoStream.simulcastStream.contentType == contentType)
    {
        sendRecordingSimulcast(targetStream, videoStream, videoStream.simulcastStream, isAdded);
    }

    if (videoStream.secondarySimulcastStream.isSet() &&
        videoStream.secondarySimulcastStream.get().contentType == contentType)
    {
        sendRecordingSimulcast(targetStream, videoStream, videoStream.secondarySimulcastStream.get(), isAdded);
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
    const auto ssrc = simulcast.levels[0].ssrc;
    if (ssrc == 0)
    {
        return;
    }

    if (targetStream.transports.empty())
    {
        logger::error("Video recording stream has zero transports. endpointHashId %zu",
            _loggableId.c_str(),
            targetStream.endpointIdHash);

        return;
    }

    memory::UniquePacket packet;
    if (isAdded)
    {
        auto outboundContextIt = targetStream.ssrcOutboundContexts.find(ssrc);
        if (outboundContextIt != targetStream.ssrcOutboundContexts.end())
        {
            if (!outboundContextIt->second.recordingOutboundDecommissioned)
            {
                // The event already was sent
                // It happens when audio is reconfigured
                // We will not send the event again
                return;
            }

            outboundContextIt->second.recordingOutboundDecommissioned = false;
            outboundContextIt->second.markedForDeletion = false;

            logger::info("Reactivating decommissioned recording outbound video ssrc %u for recording stream %zu",
                _loggableId.c_str(),
                ssrc,
                targetStream.endpointIdHash);
        }
        else
        {
            auto emplaceResult = targetStream.ssrcOutboundContexts.emplace(ssrc,
                ssrc,
                _sendAllocator,
                videoStream.rtpMap,
                bridge::RtpMap::EMPTY);

            if (!emplaceResult.second && emplaceResult.first == targetStream.ssrcOutboundContexts.end())
            {
                logger::error("Failed to create outbound context for video ssrc %u, recording stream %zu",
                    _loggableId.c_str(),
                    ssrc,
                    targetStream.endpointIdHash);
            }
            else
            {
                logger::info("Created new outbound context for video recording stream  %zu, ssrc %u",
                    _loggableId.c_str(),
                    targetStream.endpointIdHash,
                    ssrc);
            }
        }

        packet = recp::RecStreamAddedEventBuilder(_sendAllocator)
                     .setSequenceNumber(targetStream.recordingEventsOutboundContext.sequenceNumber++)
                     .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / utils::Time::ms))
                     .setSsrc(ssrc)
                     .setIsScreenSharing(simulcast.contentType == SimulcastStream::VideoContentType::SLIDES)
                     .setRtpPayloadType(static_cast<uint8_t>(videoStream.rtpMap.payloadType))
                     .setPayloadFormat(videoStream.rtpMap.format)
                     .setEndpoint(videoStream.endpointId)
                     .setWallClock(utils::Time::now())
                     .build();

        logger::info("Video ssrc %u added to recording %zu", _loggableId.c_str(), ssrc, targetStream.endpointIdHash);
    }
    else
    {
        packet = recp::RecStreamRemovedEventBuilder(_sendAllocator)
                     .setSequenceNumber(targetStream.recordingEventsOutboundContext.sequenceNumber++)
                     .setTimestamp(static_cast<uint32_t>(utils::Time::getAbsoluteTime() / utils::Time::ms))
                     .setSsrc(ssrc)
                     .build();

        auto outboundContextItr = targetStream.ssrcOutboundContexts.find(ssrc);
        if (outboundContextItr != targetStream.ssrcOutboundContexts.end())
        {
            outboundContextItr->second.recordingOutboundDecommissioned = true;
        }

        logger::info("Video ssrc %u removed from recording %zu",
            _loggableId.c_str(),
            ssrc,
            targetStream.endpointIdHash);
    }

    auto lastKey = (--targetStream.transports.end())->first;
    for (const auto& transportEntry : targetStream.transports)
    {
        auto unackedPacketsTrackerItr = targetStream.recEventUnackedPacketsTracker.find(transportEntry.first);
        if (unackedPacketsTrackerItr == targetStream.recEventUnackedPacketsTracker.end())
        {
            logger::error("RecEvent packet tracker not found. Unable to send stream event to %s",
                _loggableId.c_str(),
                transportEntry.second.getLoggableId().c_str());
            return;
        }

        memory::UniquePacket packetToSend;
        if (lastKey == transportEntry.first)
        {
            packetToSend = std::move(packet);
        }
        else
        {
            packetToSend = makeUniquePacket(_sendAllocator, *packet);
        }

        if (!packetToSend)
        {
            // This need to be improved. If we can't allocate this event, the recording
            // must fail as the we will not info about this stream
            logger::error("No space to allocate rec VideoStream%s event",
                _loggableId.c_str(),
                isAdded ? "Added" : "Removed");
            continue;
        }

        transportEntry.second.getJobQueue().addJob<RecordingSendEventJob>(std::move(packetToSend),
            transportEntry.second,
            targetStream.recordingEventsOutboundContext.packetCache,
            unackedPacketsTrackerItr->second);
    }
}

void EngineMixer::sendAudioStreamToRecording(const EngineAudioStream& audioStream, bool isAdded)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->isAudioEnabled)
        {
            sendRecordingAudioStream(*rec.second, audioStream, isAdded);
        }
    }
}

void EngineMixer::sendVideoStreamToRecording(const EngineVideoStream& videoStream, bool isAdded)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->isVideoEnabled)
        {
            sendRecordingVideoStream(*rec.second, videoStream, SimulcastStream::VideoContentType::VIDEO, isAdded);
        }

        if (rec.second->isScreenSharingEnabled)
        {
            sendRecordingVideoStream(*rec.second, videoStream, SimulcastStream::VideoContentType::SLIDES, isAdded);
        }
    }
}

void EngineMixer::removeVideoSsrcFromRecording(const EngineVideoStream& videoStream, uint32_t ssrc)
{
    for (auto& rec : _engineRecordingStreams)
    {
        if (rec.second->isVideoEnabled)
        {
            if (videoStream.simulcastStream.levels[0].ssrc == ssrc &&
                videoStream.simulcastStream.contentType == SimulcastStream::VideoContentType::VIDEO)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream.simulcastStream, false);
            }

            if (videoStream.secondarySimulcastStream.isSet() &&
                videoStream.secondarySimulcastStream.get().levels[0].ssrc == ssrc &&
                videoStream.secondarySimulcastStream.get().contentType == SimulcastStream::VideoContentType::VIDEO)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream.simulcastStream, false);
            }
        }

        if (rec.second->isScreenSharingEnabled)
        {
            if (videoStream.simulcastStream.levels[0].ssrc == ssrc &&
                videoStream.simulcastStream.contentType == SimulcastStream::VideoContentType::SLIDES)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream.simulcastStream, false);
            }

            if (videoStream.secondarySimulcastStream.isSet() &&
                videoStream.secondarySimulcastStream.get().levels[0].ssrc == ssrc &&
                videoStream.secondarySimulcastStream.get().contentType == SimulcastStream::VideoContentType::SLIDES)
            {
                sendRecordingSimulcast(*rec.second, videoStream, videoStream.secondarySimulcastStream.get(), false);
            }
        }
    }
}

void EngineMixer::processRecordingUnackedPackets(const uint64_t timestamp)
{
    if (utils::Time::diffLT(_lastRecordingAckProcessed, timestamp, RECUNACK_PROCESS_INTERVAL))
    {
        return;
    }
    _lastRecordingAckProcessed = timestamp;

    for (auto& engineRecordingStreamEntry : _engineRecordingStreams)
    {
        auto engineRecordingStream = engineRecordingStreamEntry.second;
        for (auto& recEventMissingPacketsTrackerEntry : engineRecordingStream->recEventUnackedPacketsTracker)
        {
            auto& recEventMissingPacketsTracker = recEventMissingPacketsTrackerEntry.second;

            auto transportItr = engineRecordingStream->transports.find(recEventMissingPacketsTrackerEntry.first);
            if (transportItr == engineRecordingStream->transports.end())
            {
                continue;
            }

            auto& transport = transportItr->second;
            transport.getJobQueue().addJob<ProcessUnackedRecordingEventPacketsJob>(
                engineRecordingStream->recordingEventsOutboundContext,
                recEventMissingPacketsTracker,
                transport,
                _sendAllocator);
        }
    }
}

void EngineMixer::checkRecordingPacketCounters(const uint64_t timestamp)
{
    // We will give 15 seconds before delete outbound context since it was marked for deletion
    // because we can still receive some NACK or need to resend some recording events if smb hasn't
    // receive the ACKs. The recording events are especially important to recording to be converted properly
    static constexpr uint64_t decommissionedTimeout = 15 * utils::Time::sec;
    for (auto& recordingStreamEntry : _engineRecordingStreams)
    {
        const auto recordingStreamIdHash = recordingStreamEntry.first;
        for (auto& outboundContextEntry : recordingStreamEntry.second->ssrcOutboundContexts)
        {
            auto& outboundContext = outboundContextEntry.second;

            const bool isDecommissioned =
                outboundContext.recordingOutboundDecommissioned && !outboundContext.markedForDeletion;

            if (isDecommissioned && utils::Time::diffGT(outboundContext.lastSendTime, timestamp, decommissionedTimeout))
            {
                logger::info("Marking for deletion decommissioned recording outbound context ssrc %u, rec "
                             "endpointIdHash %zu",
                    _loggableId.c_str(),
                    outboundContextEntry.first,
                    recordingStreamIdHash);

                const auto ssrc = outboundContextEntry.first;
                outboundContext.markedForDeletion = true;
                auto& outboundContextsFinalizerCounter = recordingStreamEntry.second->outboundContextsFinalizerCounter;
                auto counterIt = outboundContextsFinalizerCounter.find(ssrc);
                if (counterIt == outboundContextsFinalizerCounter.end())
                {
                    auto pair = outboundContextsFinalizerCounter.emplace(ssrc, 0);
                    if (!pair.second)
                    {
                        logger::error("No space to emplace recording counter for endpoint %zu, ssrc %u",
                            _loggableId.c_str(),
                            recordingStreamIdHash,
                            ssrc);
                        outboundContext.markedForDeletion = false;
                        // This will reschedule new execution in 5 seconds
                        outboundContext.lastSendTime = timestamp - decommissionedTimeout + 5 * utils::Time::sec;
                        return;
                    }

                    counterIt = pair.first;
                }

                std::atomic_uint32_t& finalizeCounter = counterIt->second;
                for (auto& transportEntry : recordingStreamEntry.second->transports)
                {
                    transportEntry.second.getJobQueue().addJob<FinalizeRecordingOutboundContextJob>(*this,
                        transportEntry.second,
                        recordingStreamIdHash,
                        _engineSyncContext,
                        ssrc,
                        finalizeCounter);
                }
            }
        }
    }
}

bool EngineMixer::asyncAddRecordingStream(EngineRecordingStream* engineRecordingStream)
{
    return post(utils::bind(&EngineMixer::addRecordingStream, this, engineRecordingStream));
}

bool EngineMixer::asyncAddTransportToRecordingStream(const size_t streamIdHash,
    transport::RecordingTransport& transport,
    UnackedPacketsTracker& recUnackedPacketsTracker)
{
    return post(utils::bind(&EngineMixer::addTransportToRecordingStream,
        this,
        streamIdHash,
        std::ref(transport),
        std::ref(recUnackedPacketsTracker)));
}

bool EngineMixer::asyncRecordingStart(EngineRecordingStream& stream, const RecordingDescription& desc)
{
    return post(utils::bind(&EngineMixer::recordingStart, this, std::ref(stream), desc));
}

bool EngineMixer::asyncUpdateRecordingStreamModalities(EngineRecordingStream& engineRecordingStream,
    bool isAudioEnabled,
    bool isVideoEnabled,
    bool isScreenSharingEnabled)
{
    return post(utils::bind(&EngineMixer::updateRecordingStreamModalities,
        this,
        std::ref(engineRecordingStream),
        isAudioEnabled,
        isVideoEnabled,
        isScreenSharingEnabled));
}

bool EngineMixer::asyncStartRecordingTransport(transport::RecordingTransport& transport)
{
    return post(utils::bind(&EngineMixer::startRecordingTransport, this, std::ref(transport)));
}

bool EngineMixer::asyncStopRecording(EngineRecordingStream& stream, const RecordingDescription& desc)
{
    return post([this, &stream, &desc]() {
        this->stopRecording(stream, desc);
        _messageListener.asyncEngineRecordingStopped(*this, desc);
    });
}

bool EngineMixer::asyncAddRecordingRtpPacketCache(const uint32_t ssrc,
    const size_t endpointIdHash,
    PacketCache* packetCache)
{
    return post(utils::bind(&EngineMixer::addRecordingRtpPacketCache, this, ssrc, endpointIdHash, packetCache));
}

bool EngineMixer::asyncRemoveTransportFromRecordingStream(const size_t streamIdHash, const size_t endpointIdHash)
{
    return post(utils::bind(&EngineMixer::removeTransportFromRecordingStream, this, streamIdHash, endpointIdHash));
}

bool EngineMixer::asyncRemoveRecordingStream(const EngineRecordingStream& engineRecordingStream)
{
    return post([this, &engineRecordingStream]() {
        this->removeRecordingStream(engineRecordingStream);
        _messageListener.asyncRecordingStreamRemoved(*this, engineRecordingStream);
    });
}

} // namespace bridge
