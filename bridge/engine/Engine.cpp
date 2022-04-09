#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMessageListener.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include "utils/Pacer.h"
#include <cassert>

namespace
{

const auto intervalNs = 1000000UL * bridge::EngineMixer::iterationDurationMs;

}

namespace bridge
{

Engine::Engine()
    : _messageListener(nullptr),
      _running(true),
      _pendingCommands(1024),
      _tickCounter(0),
      _thread([this] { this->run(); })
{
    if (concurrency::setPriority(_thread, concurrency::Priority::RealTime))
    {
        logger::info("Successfully set thread priority to realtime.", "Engine");
    }
}

void Engine::setMessageListener(EngineMessageListener* messageListener)
{
    _messageListener = messageListener;
}

void Engine::stop()
{
    _running = false;
    _thread.join();
    logger::debug("Engine stopped", "Engine");
}

void Engine::run()
{
    logger::debug("Engine started", "Engine");
    concurrency::setThreadName("Engine");
    utils::Pacer pacer(intervalNs);
    EngineStats::EngineStats currentStatSample;

    uint64_t previousPollTime = utils::Time::getAbsoluteTime() - utils::Time::sec * 2;
    while (_running)
    {
        const auto engineIterationStartTimestamp = utils::Time::getAbsoluteTime();
        pacer.tick(engineIterationStartTimestamp);

        const auto numPendingCommands = std::min(_pendingCommands.size(), 100LU);

        for (size_t i = 0; i < numPendingCommands; ++i)
        {
            EngineCommand::Command nextCommand;
            if (!_pendingCommands.pop(nextCommand))
            {
                break;
            }

            switch (nextCommand._type)
            {
            case EngineCommand::Type::AddMixer:
                addMixer(nextCommand);
                break;

            case EngineCommand::Type::RemoveMixer:
                removeMixer(nextCommand);
                break;

            case EngineCommand::Type::AddAudioStream:
                addAudioStream(nextCommand);
                break;

            case EngineCommand::Type::RemoveAudioStream:
                removeAudioStream(nextCommand);
                break;

            case EngineCommand::Type::AddAudioBuffer:
                addAudioBuffer(nextCommand);
                break;

            case EngineCommand::Type::AddVideoStream:
                addVideoStream(nextCommand);
                break;

            case EngineCommand::Type::RemoveVideoStream:
                removeVideoStream(nextCommand);
                break;

            case EngineCommand::Type::UpdateRecordingStreamModalities:
                updateRecordingStreamModalities(nextCommand);
                break;

            case EngineCommand::Type::AddDataStream:
                addDataStream(nextCommand);
                break;

            case EngineCommand::Type::RemoveDataStream:
                removeDataStream(nextCommand);
                break;

            case EngineCommand::Type::StartTransport:
                startTransport(nextCommand);
                break;

            case EngineCommand::Type::StartRecordingTransport:
                startRecordingTransport(nextCommand);
                break;

            case EngineCommand::Type::ReconfigureAudioStream:
                reconfigureAudioStream(nextCommand);
                break;

            case EngineCommand::Type::ReconfigureVideoStream:
            case EngineCommand::Type::ReconfigureVideoStreamSecondary:
                reconfigureVideoStream(nextCommand);
                break;

            case EngineCommand::Type::AddVideoPacketCache:
                addVideoPacketCache(nextCommand);
                break;

            case EngineCommand::Type::SctpControl:
                processSctpControl(nextCommand);
                break;

            case EngineCommand::Type::PinEndpoint:
                pinEndpoint(nextCommand);
                break;

            case EngineCommand::Type::EndpointMessage:
                sendEndpointMessage(nextCommand);
                break;

            case EngineCommand::Type::AddRecordingStream:
                addRecordingStream(nextCommand);
                break;

            case EngineCommand::Type::RemoveRecordingStream:
                removeRecordingStream(nextCommand);
                break;

            case EngineCommand::Type::StartRecording:
                startRecording(nextCommand);
                break;

            case EngineCommand::Type::StopRecording:
                stopRecording(nextCommand);
                break;

            case EngineCommand::Type::AddRecordingRtpPacketCache:
                addRecordingRtpPacketCache(nextCommand);
                break;

            case EngineCommand::Type::AddTransportToRecordingStream:
                addTransportToRecordingStream(nextCommand);
                break;
            case EngineCommand::Type::RemoveTransportFromRecordingStream:
                removeTransportFromRecordingStream(nextCommand);
                break;

            default:
                assert(false);
                break;
            }
        }

        {
            auto mixerEntry = _mixers.head();
            while (mixerEntry)
            {
                assert(mixerEntry->_data);
                mixerEntry->_data->run(engineIterationStartTimestamp);
                mixerEntry = mixerEntry->_next;
            }
        }

        if (++_tickCounter % STATS_UPDATE_TICKS == 0)
        {
            uint64_t pollTime = utils::Time::getAbsoluteTime();
            currentStatSample.activeMixers = EngineStats::MixerStats();
            for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
            {
                currentStatSample.activeMixers += mixerEntry->_data->gatherStats(engineIterationStartTimestamp);
            }

            currentStatSample.pollPeriodMs =
                static_cast<uint32_t>(std::max(uint64_t(1), (pollTime - previousPollTime) / uint64_t(1000000)));
            _stats.write(currentStatSample);
            previousPollTime = pollTime;
        }

        auto toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());

        if (toSleep > 0)
        {
            utils::Time::nanoSleep(toSleep);
        }
        else
        {
            ++currentStatSample.timeSlipCount;
        }
    }
}

void Engine::pushCommand(const EngineCommand::Command& command)
{
    auto result = _pendingCommands.push(command);
    assert(result);
    if (!result)
    {
        logger::error("Unable to add command to Engine, queue full", "Engine");
    }
}

void Engine::addMixer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddMixer);
    assert(nextCommand._command.addMixer._mixer);

    logger::debug("Adding mixer %s", "Engine", nextCommand._command.addMixer._mixer->getLoggableId().c_str());
    if (!_mixers.pushToTail(nextCommand._command.addMixer._mixer))
    {
        logger::error("Unable to add EngineMixer %s to Engine",
            "Engine",
            nextCommand._command.addMixer._mixer->getLoggableId().c_str());

        EngineMessage::Message message = {EngineMessage::Type::MixerRemoved};
        message._command.mixerRemoved._mixer = nextCommand._command.addMixer._mixer;
        _messageListener->onMessage(message);

        return;
    }
}

void Engine::removeMixer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::RemoveMixer);
    auto& command = nextCommand._command.removeMixer;
    assert(command._mixer);

    logger::debug("Removing mixer %s", "Engine", command._mixer->getLoggableId().c_str());
    command._mixer->clear();
    if (!_mixers.remove(command._mixer))
    {
        logger::error("Unable to remove EngineMixer %s from Engine", "Engine", command._mixer->getLoggableId().c_str());
    }

    EngineMessage::Message message = {EngineMessage::Type::MixerRemoved};
    message._command.mixerRemoved._mixer = command._mixer;
    _messageListener->onMessage(message);
}

void Engine::addAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddAudioStream);
    assert(nextCommand._command.addAudioStream._mixer);
    assert(nextCommand._command.addAudioStream._engineStream);

    auto mixer = nextCommand._command.addAudioStream._mixer;
    mixer->addAudioStream(nextCommand._command.addAudioStream._engineStream);
}

void Engine::removeAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::RemoveAudioStream);
    assert(nextCommand._command.removeAudioStream._mixer);
    assert(nextCommand._command.removeAudioStream._engineStream);

    logger::debug("Remove audioStream, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.removeAudioStream._mixer->getLoggableId().c_str(),
        nextCommand._command.removeAudioStream._engineStream->_transport.getLoggableId().c_str());

    auto mixer = nextCommand._command.removeAudioStream._mixer;
    mixer->removeAudioStream(nextCommand._command.removeAudioStream._engineStream);
}

void Engine::addAudioBuffer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddAudioBuffer);
    assert(nextCommand._command.addAudioBuffer._mixer);
    assert(nextCommand._command.addAudioBuffer._audioBuffer);

    logger::debug("Add ssrc audio buffer mixer %s, ssrc %u",
        "Engine",
        nextCommand._command.addAudioBuffer._mixer->getLoggableId().c_str(),
        nextCommand._command.addAudioBuffer._ssrc);
    nextCommand._command.addAudioBuffer._mixer->addAudioBuffer(nextCommand._command.addAudioBuffer._ssrc,
        nextCommand._command.addAudioBuffer._audioBuffer);
}

void Engine::addVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddVideoStream);
    assert(nextCommand._command.addVideoStream._mixer);
    assert(nextCommand._command.addVideoStream._engineStream);

    auto mixer = nextCommand._command.addVideoStream._mixer;
    mixer->addVideoStream(nextCommand._command.addVideoStream._engineStream);
}

void Engine::removeVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::RemoveVideoStream);
    assert(nextCommand._command.removeVideoStream._mixer);
    assert(nextCommand._command.removeVideoStream._engineStream);

    logger::debug("Remove videoStream, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.removeVideoStream._mixer->getLoggableId().c_str(),
        nextCommand._command.removeVideoStream._engineStream->_transport.getLoggableId().c_str());

    auto mixer = nextCommand._command.removeVideoStream._mixer;
    mixer->removeVideoStream(nextCommand._command.removeVideoStream._engineStream);
}

void Engine::addRecordingStream(EngineCommand::Command& command)
{
    assert(command._type == EngineCommand::Type::AddRecordingStream);
    assert(command._command.removeRecordingStream._mixer);
    assert(command._command.removeRecordingStream._recordingStream);

    auto mixer = command._command.addRecordingStream._mixer;
    mixer->addRecordingStream(command._command.addRecordingStream._recordingStream);
}

void Engine::removeRecordingStream(EngineCommand::Command& command)
{
    assert(command._type == EngineCommand::Type::RemoveRecordingStream);
    assert(command._command.removeRecordingStream._mixer);
    assert(command._command.removeRecordingStream._recordingStream);

    logger::debug("Remove recordingStream, mixer %s",
        "Engine",
        command._command.removeRecordingStream._mixer->getLoggableId().c_str());

    auto mixer = command._command.addRecordingStream._mixer;
    mixer->removeRecordingStream(command._command.addRecordingStream._recordingStream);

    EngineMessage::Message message = {EngineMessage::Type::RecordingStreamRemoved};
    message._command.recordingStreamRemoved._mixer = command._command.removeRecordingStream._mixer;
    message._command.recordingStreamRemoved._engineStream = command._command.removeRecordingStream._recordingStream;

    _messageListener->onMessage(message);
}

void Engine::updateRecordingStreamModalities(EngineCommand::Command& command)
{
    assert(command._type == EngineCommand::Type::UpdateRecordingStreamModalities);
    assert(command._command.updateRecordingStreamModalities._mixer);
    assert(command._command.updateRecordingStreamModalities._recordingStream);

    auto& updateModalitiesCommand = command._command.updateRecordingStreamModalities;

    logger::debug("Update recordingStream modalities, mixer %s, stream: %s audio: %s, video: %s",
        "Engine",
        updateModalitiesCommand._mixer->getLoggableId().c_str(),
        updateModalitiesCommand._recordingStream->_id.c_str(),
        updateModalitiesCommand._isAudioEnabled ? "enabled" : "disabled",
        updateModalitiesCommand._isVideoEnabled ? "enabled" : "disabled");

    auto mixer = updateModalitiesCommand._mixer;
    mixer->updateRecordingStreamModalities(updateModalitiesCommand._recordingStream,
        updateModalitiesCommand._isAudioEnabled,
        updateModalitiesCommand._isVideoEnabled,
        updateModalitiesCommand._isScreenSharingEnabled);
}

void Engine::addDataStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddDataStream);
    assert(nextCommand._command.addDataStream._mixer);
    assert(nextCommand._command.addDataStream._engineStream);

    auto mixer = nextCommand._command.addDataStream._mixer;
    mixer->addDataSteam(nextCommand._command.addDataStream._engineStream);
}

void Engine::removeDataStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::RemoveDataStream);
    assert(nextCommand._command.removeDataStream._mixer);
    assert(nextCommand._command.removeDataStream._engineStream);

    logger::debug("Remove dataStream, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.removeDataStream._mixer->getLoggableId().c_str(),
        nextCommand._command.removeDataStream._engineStream->_transport.getLoggableId().c_str());

    auto mixer = nextCommand._command.removeDataStream._mixer;
    mixer->removeDataStream(nextCommand._command.removeDataStream._engineStream);

    EngineMessage::Message message = {EngineMessage::Type::DataStreamRemoved};
    message._command.dataStreamRemoved._mixer = nextCommand._command.removeDataStream._mixer;
    message._command.dataStreamRemoved._engineStream = nextCommand._command.removeDataStream._engineStream;

    _messageListener->onMessage(message);
}

void Engine::startTransport(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::StartTransport);
    assert(nextCommand._command.startTransport._mixer);
    assert(nextCommand._command.startTransport._transport);

    logger::debug("Start transport, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.startTransport._mixer->getLoggableId().c_str(),
        nextCommand._command.startTransport._transport->getLoggableId().c_str());

    auto mixer = nextCommand._command.startTransport._mixer;
    mixer->startTransport(nextCommand._command.startTransport._transport);
}

void Engine::startRecordingTransport(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::StartRecordingTransport);
    assert(nextCommand._command.startRecordingTransport._mixer);
    assert(nextCommand._command.startRecordingTransport._transport);

    logger::debug("Start recording transport, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.startRecordingTransport._mixer->getLoggableId().c_str(),
        nextCommand._command.startRecordingTransport._transport->getLoggableId().c_str());

    auto mixer = nextCommand._command.startRecordingTransport._mixer;
    mixer->startRecordingTransport(nextCommand._command.startRecordingTransport._transport);
}

void Engine::reconfigureAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::ReconfigureAudioStream);
    assert(nextCommand._command.reconfigureAudioStream._mixer);
    assert(nextCommand._command.reconfigureAudioStream._transport);

    logger::debug("Reconfigure audio stream, mixer %s, transport id %s",
        "Engine",
        nextCommand._command.reconfigureAudioStream._mixer->getLoggableId().c_str(),
        nextCommand._command.reconfigureAudioStream._transport->getLoggableId().c_str());

    auto mixer = nextCommand._command.reconfigureAudioStream._mixer;
    mixer->reconfigureAudioStream(nextCommand._command.reconfigureAudioStream._transport,
        nextCommand._command.reconfigureAudioStream._remoteSsrc);
}

void Engine::reconfigureVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::ReconfigureVideoStream ||
        nextCommand._type == EngineCommand::Type::ReconfigureVideoStreamSecondary);

    if (nextCommand._type == EngineCommand::Type::ReconfigureVideoStream)
    {
        const auto& reconfigureVideoStream = nextCommand._command.reconfigureVideoStream;
        assert(reconfigureVideoStream._mixer);
        assert(reconfigureVideoStream._transport);

        logger::debug("Reconfigure video stream, mixer %s, endpointIdHash %lu, whitelist %c %u %u %u",
            "Engine",
            reconfigureVideoStream._mixer->getLoggableId().c_str(),
            reconfigureVideoStream._transport->getEndpointIdHash(),
            reconfigureVideoStream._ssrcWhitelist._enabled ? 't' : 'f',
            reconfigureVideoStream._ssrcWhitelist._numSsrcs,
            reconfigureVideoStream._ssrcWhitelist._ssrcs[0],
            reconfigureVideoStream._ssrcWhitelist._ssrcs[1]);

        auto mixer = reconfigureVideoStream._mixer;
        mixer->reconfigureVideoStream(reconfigureVideoStream._transport,
            reconfigureVideoStream._ssrcWhitelist,
            reconfigureVideoStream._simulcastStream);
    }
    else if (nextCommand._type == EngineCommand::Type::ReconfigureVideoStreamSecondary)
    {
        const auto& reconfigureVideoStreamSecondary = nextCommand._command.reconfigureVideoStreamSecondary;
        assert(reconfigureVideoStreamSecondary._mixer);
        assert(reconfigureVideoStreamSecondary._transport);

        logger::debug("Reconfigure video stream with secondary, mixer %s, endpointIdHash %lu, whitelist %c %u %u %u",
            "Engine",
            reconfigureVideoStreamSecondary._mixer->getLoggableId().c_str(),
            reconfigureVideoStreamSecondary._transport->getEndpointIdHash(),
            reconfigureVideoStreamSecondary._ssrcWhitelist._enabled ? 't' : 'f',
            reconfigureVideoStreamSecondary._ssrcWhitelist._numSsrcs,
            reconfigureVideoStreamSecondary._ssrcWhitelist._ssrcs[0],
            reconfigureVideoStreamSecondary._ssrcWhitelist._ssrcs[1]);

        auto mixer = reconfigureVideoStreamSecondary._mixer;
        mixer->reconfigureVideoStream(nextCommand._command.reconfigureVideoStreamSecondary._transport,
            reconfigureVideoStreamSecondary._ssrcWhitelist,
            reconfigureVideoStreamSecondary._simulcastStream,
            &reconfigureVideoStreamSecondary._secondarySimulcastStream);
    }
    else
    {
        assert(false);
    }
}

void Engine::addVideoPacketCache(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddVideoPacketCache);
    assert(nextCommand._command.addVideoPacketCache._mixer);
    assert(nextCommand._command.addVideoPacketCache._videoPacketCache);

    logger::debug("Add videoPacketCache, mixer %s, ssrc %u, endpointIdHash %lu",
        "Engine",
        nextCommand._command.addVideoPacketCache._mixer->getLoggableId().c_str(),
        nextCommand._command.addVideoPacketCache._ssrc,
        nextCommand._command.addVideoPacketCache._endpointIdHash);

    auto mixer = nextCommand._command.addVideoPacketCache._mixer;
    mixer->addVideoPacketCache(nextCommand._command.addVideoPacketCache._ssrc,
        nextCommand._command.addVideoPacketCache._endpointIdHash,
        nextCommand._command.addVideoPacketCache._videoPacketCache);
}

void Engine::processSctpControl(EngineCommand::Command& command)
{
    auto& sctpCommand = command._command.sctpControl;
    sctpCommand._mixer->handleSctpControl(sctpCommand._endpointIdHash,
        memory::PacketPtr(sctpCommand._message, sctpCommand._allocator->getDeleter()));
}

void Engine::pinEndpoint(EngineCommand::Command& command)
{
    auto& pinCommand = command._command.pinEndpoint;
    pinCommand._mixer->pinEndpoint(pinCommand._endpointIdHash, pinCommand._pinnedEndpointIdHash);
}

void Engine::startRecording(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::StartRecording);
    assert(nextCommand._command.startRecording._mixer);
    assert(nextCommand._command.startRecording._recordingDesc);
    assert(nextCommand._command.startRecording._recordingStream);

    auto mixer = nextCommand._command.startRecording._mixer;
    mixer->recordingStart(nextCommand._command.startRecording._recordingStream,
        nextCommand._command.startRecording._recordingDesc);
}

void Engine::stopRecording(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::StopRecording);
    assert(nextCommand._command.stopRecording._mixer);
    assert(nextCommand._command.stopRecording._recordingDesc);
    assert(nextCommand._command.stopRecording._recordingStream);

    auto mixer = nextCommand._command.stopRecording._mixer;
    mixer->recordingStop(nextCommand._command.stopRecording._recordingStream,
        nextCommand._command.stopRecording._recordingDesc);

    EngineMessage::Message message = {EngineMessage::Type::RecordingStopped};
    message._command.recordingStopped._mixer = nextCommand._command.removeDataStream._mixer;
    message._command.recordingStopped._recordingDesc = nextCommand._command.stopRecording._recordingDesc;

    _messageListener->onMessage(message);
}

void Engine::addRecordingRtpPacketCache(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddRecordingRtpPacketCache);
    assert(nextCommand._command.addRecordingRtpPacketCache._mixer);
    assert(nextCommand._command.addRecordingRtpPacketCache._packetCache);

    logger::debug("Add RecordingRtpPacketCache, mixer %s, ssrc %u",
        "Engine",
        nextCommand._command.addRecordingRtpPacketCache._mixer->getLoggableId().c_str(),
        nextCommand._command.addRecordingRtpPacketCache._ssrc);

    auto mixer = nextCommand._command.addRecordingRtpPacketCache._mixer;
    mixer->addRecordingRtpPacketCache(nextCommand._command.addRecordingRtpPacketCache._ssrc,
        nextCommand._command.addRecordingRtpPacketCache._endpointIdHash,
        nextCommand._command.addRecordingRtpPacketCache._packetCache);
}

void Engine::addTransportToRecordingStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::AddTransportToRecordingStream);
    assert(nextCommand._command.addTransportToRecordingStream._mixer);
    assert(nextCommand._command.addTransportToRecordingStream._transport);
    assert(nextCommand._command.addTransportToRecordingStream._recUnackedPacketsTracker);

    auto mixer = nextCommand._command.addTransportToRecordingStream._mixer;
    mixer->addTransportToRecordingStream(nextCommand._command.addTransportToRecordingStream._streamIdHash,
        nextCommand._command.addTransportToRecordingStream._transport,
        nextCommand._command.addTransportToRecordingStream._recUnackedPacketsTracker);
}

void Engine::removeTransportFromRecordingStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand._type == EngineCommand::Type::RemoveTransportFromRecordingStream);
    assert(nextCommand._command.removeTransportFromRecordingStream._mixer);
    assert(nextCommand._command.removeTransportFromRecordingStream._streamIdHash);
    assert(nextCommand._command.removeTransportFromRecordingStream._endpointIdHash);

    auto mixer = nextCommand._command.removeTransportFromRecordingStream._mixer;
    mixer->removeTransportFromRecordingStream(nextCommand._command.removeTransportFromRecordingStream._streamIdHash,
        nextCommand._command.removeTransportFromRecordingStream._endpointIdHash);
}

EngineStats::EngineStats Engine::getStats()
{
    EngineStats::EngineStats stats;
    _stats.read(stats);
    return stats;
}

void Engine::sendEndpointMessage(EngineCommand::Command& command)
{
    auto& endpointMessageCommand = command._command.endpointMessage;
    endpointMessageCommand._mixer->sendEndpointMessage(endpointMessageCommand._toEndpointIdHash,
        endpointMessageCommand._fromEndpointIdHash,
        endpointMessageCommand._message);
}

} // namespace bridge
