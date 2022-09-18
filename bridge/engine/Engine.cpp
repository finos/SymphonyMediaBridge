#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMessageListener.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include "utils/CheckedCast.h"
#include "utils/Pacer.h"
#include <cassert>

namespace
{

const auto intervalNs = 1000000UL * bridge::EngineMixer::iterationDurationMs;

}

namespace bridge
{

Engine::Engine(const config::Config& config)
    : _config(config),
      _messageListener(nullptr),
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

            switch (nextCommand.type)
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
            case EngineCommand::Type::AddBarbell:
                addBarbell(nextCommand);
                break;
            case EngineCommand::Type::RemoveBarbell:
                nextCommand.command.removeBarbell.mixer->removeBarbell(nextCommand.command.removeBarbell.idHash);
                break;
            default:
                assert(false);
                break;
            }
        }

        for (auto mixerEntry = _mixers.head(); mixerEntry; mixerEntry = mixerEntry->_next)
        {
            assert(mixerEntry->_data);
            mixerEntry->_data->run(engineIterationStartTimestamp);
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
        if (toSleep <= 0)
        {
            ++currentStatSample.timeSlipCount;
        }

        while (toSleep > 0)
        {
            for (auto mixerEntry = _mixers.head(); toSleep > int64_t(utils::Time::ms) && mixerEntry;
                 mixerEntry = mixerEntry->_next)
            {
                assert(mixerEntry->_data);
                mixerEntry->_data->forwardPackets(utils::Time::getAbsoluteTime());
            }
            toSleep = pacer.timeToNextTick(utils::Time::getAbsoluteTime());
            if (toSleep > 0)
            {
                utils::Time::nanoSleep(
                    std::min(utils::checkedCast<uint64_t>(toSleep), utils::Time::ms * _config.rtpForwardInterval));
            }
        }
    }
}

void Engine::pushCommand(EngineCommand::Command&& command)
{
    auto result = _pendingCommands.push(std::move(command));
    assert(result);
    if (!result)
    {
        logger::error("Unable to add command to Engine, queue full", "Engine");
    }
}

void Engine::addMixer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddMixer);
    assert(nextCommand.command.addMixer.mixer);

    logger::debug("Adding mixer %s", "Engine", nextCommand.command.addMixer.mixer->getLoggableId().c_str());
    if (!_mixers.pushToTail(nextCommand.command.addMixer.mixer))
    {
        logger::error("Unable to add EngineMixer %s to Engine",
            "Engine",
            nextCommand.command.addMixer.mixer->getLoggableId().c_str());

        EngineMessage::Message message = {EngineMessage::Type::MixerRemoved};
        message.command.mixerRemoved.mixer = nextCommand.command.addMixer.mixer;
        _messageListener->onMessage(std::move(message));

        return;
    }
}

void Engine::removeMixer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::RemoveMixer);
    auto& command = nextCommand.command.removeMixer;
    assert(command.mixer);

    logger::debug("Removing mixer %s", "Engine", command.mixer->getLoggableId().c_str());
    command.mixer->clear();
    if (!_mixers.remove(command.mixer))
    {
        logger::error("Unable to remove EngineMixer %s from Engine", "Engine", command.mixer->getLoggableId().c_str());
    }

    EngineMessage::Message message = {EngineMessage::Type::MixerRemoved};
    message.command.mixerRemoved.mixer = command.mixer;
    _messageListener->onMessage(std::move(message));
}

void Engine::addAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddAudioStream);
    assert(nextCommand.command.addAudioStream.mixer);
    assert(nextCommand.command.addAudioStream.engineStream);

    auto mixer = nextCommand.command.addAudioStream.mixer;
    mixer->addAudioStream(nextCommand.command.addAudioStream.engineStream);
}

void Engine::removeAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::RemoveAudioStream);
    assert(nextCommand.command.removeAudioStream.mixer);
    assert(nextCommand.command.removeAudioStream.engineStream);

    logger::debug("Remove audioStream, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.removeAudioStream.mixer->getLoggableId().c_str(),
        nextCommand.command.removeAudioStream.engineStream->transport.getLoggableId().c_str());

    auto mixer = nextCommand.command.removeAudioStream.mixer;
    mixer->removeStream(nextCommand.command.removeAudioStream.engineStream);
}

void Engine::addAudioBuffer(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddAudioBuffer);
    assert(nextCommand.command.addAudioBuffer.mixer);
    assert(nextCommand.command.addAudioBuffer.audioBuffer);

    logger::debug("Add ssrc audio buffer mixer %s, ssrc %u",
        "Engine",
        nextCommand.command.addAudioBuffer.mixer->getLoggableId().c_str(),
        nextCommand.command.addAudioBuffer.ssrc);
    nextCommand.command.addAudioBuffer.mixer->addAudioBuffer(nextCommand.command.addAudioBuffer.ssrc,
        nextCommand.command.addAudioBuffer.audioBuffer);
}

void Engine::addVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddVideoStream);
    assert(nextCommand.command.addVideoStream.mixer);
    assert(nextCommand.command.addVideoStream.engineStream);

    auto mixer = nextCommand.command.addVideoStream.mixer;
    mixer->addVideoStream(nextCommand.command.addVideoStream.engineStream);
}

void Engine::removeVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::RemoveVideoStream);
    assert(nextCommand.command.removeVideoStream.mixer);
    assert(nextCommand.command.removeVideoStream.engineStream);

    logger::debug("Remove videoStream, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.removeVideoStream.mixer->getLoggableId().c_str(),
        nextCommand.command.removeVideoStream.engineStream->transport.getLoggableId().c_str());

    auto mixer = nextCommand.command.removeVideoStream.mixer;
    mixer->removeStream(nextCommand.command.removeVideoStream.engineStream);
}

void Engine::addRecordingStream(EngineCommand::Command& command)
{
    assert(command.type == EngineCommand::Type::AddRecordingStream);
    assert(command.command.removeRecordingStream.mixer);
    assert(command.command.removeRecordingStream.recordingStream);

    auto mixer = command.command.addRecordingStream.mixer;
    mixer->addRecordingStream(command.command.addRecordingStream.recordingStream);
}

void Engine::removeRecordingStream(EngineCommand::Command& command)
{
    assert(command.type == EngineCommand::Type::RemoveRecordingStream);
    assert(command.command.removeRecordingStream.mixer);
    assert(command.command.removeRecordingStream.recordingStream);

    logger::debug("Remove recordingStream, mixer %s",
        "Engine",
        command.command.removeRecordingStream.mixer->getLoggableId().c_str());

    auto mixer = command.command.addRecordingStream.mixer;
    mixer->removeRecordingStream(command.command.addRecordingStream.recordingStream);

    EngineMessage::Message message = {EngineMessage::Type::RecordingStreamRemoved};
    message.command.recordingStreamRemoved.mixer = command.command.removeRecordingStream.mixer;
    message.command.recordingStreamRemoved.engineStream = command.command.removeRecordingStream.recordingStream;

    _messageListener->onMessage(std::move(message));
}

void Engine::updateRecordingStreamModalities(EngineCommand::Command& command)
{
    assert(command.type == EngineCommand::Type::UpdateRecordingStreamModalities);
    assert(command.command.updateRecordingStreamModalities.mixer);
    assert(command.command.updateRecordingStreamModalities.recordingStream);

    auto& updateModalitiesCommand = command.command.updateRecordingStreamModalities;

    logger::debug("Update recordingStream modalities, mixer %s, stream: %s audio: %s, video: %s",
        "Engine",
        updateModalitiesCommand.mixer->getLoggableId().c_str(),
        updateModalitiesCommand.recordingStream->id.c_str(),
        updateModalitiesCommand.audioEnabled ? "enabled" : "disabled",
        updateModalitiesCommand.videoEnabled ? "enabled" : "disabled");

    auto mixer = updateModalitiesCommand.mixer;
    mixer->updateRecordingStreamModalities(updateModalitiesCommand.recordingStream,
        updateModalitiesCommand.audioEnabled,
        updateModalitiesCommand.videoEnabled,
        updateModalitiesCommand.screenSharingEnabled);
}

void Engine::addDataStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddDataStream);
    assert(nextCommand.command.addDataStream.mixer);
    assert(nextCommand.command.addDataStream.engineStream);

    auto mixer = nextCommand.command.addDataStream.mixer;
    mixer->addDataSteam(nextCommand.command.addDataStream.engineStream);
}

void Engine::removeDataStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::RemoveDataStream);
    assert(nextCommand.command.removeDataStream.mixer);
    assert(nextCommand.command.removeDataStream.engineStream);

    logger::debug("Remove dataStream, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.removeDataStream.mixer->getLoggableId().c_str(),
        nextCommand.command.removeDataStream.engineStream->transport.getLoggableId().c_str());

    auto mixer = nextCommand.command.removeDataStream.mixer;
    mixer->removeStream(nextCommand.command.removeDataStream.engineStream);
}

void Engine::startTransport(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::StartTransport);
    assert(nextCommand.command.startTransport.mixer);
    assert(nextCommand.command.startTransport.transport);

    logger::debug("Start transport, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.startTransport.mixer->getLoggableId().c_str(),
        nextCommand.command.startTransport.transport->getLoggableId().c_str());

    auto mixer = nextCommand.command.startTransport.mixer;
    mixer->startTransport(nextCommand.command.startTransport.transport);
}

void Engine::startRecordingTransport(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::StartRecordingTransport);
    assert(nextCommand.command.startRecordingTransport.mixer);
    assert(nextCommand.command.startRecordingTransport.transport);

    logger::debug("Start recording transport, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.startRecordingTransport.mixer->getLoggableId().c_str(),
        nextCommand.command.startRecordingTransport.transport->getLoggableId().c_str());

    auto mixer = nextCommand.command.startRecordingTransport.mixer;
    mixer->startRecordingTransport(nextCommand.command.startRecordingTransport.transport);
}

void Engine::reconfigureAudioStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::ReconfigureAudioStream);
    assert(nextCommand.command.reconfigureAudioStream.mixer);
    assert(nextCommand.command.reconfigureAudioStream.transport);

    logger::debug("Reconfigure audio stream, mixer %s, transport id %s",
        "Engine",
        nextCommand.command.reconfigureAudioStream.mixer->getLoggableId().c_str(),
        nextCommand.command.reconfigureAudioStream.transport->getLoggableId().c_str());

    auto mixer = nextCommand.command.reconfigureAudioStream.mixer;
    mixer->reconfigureAudioStream(nextCommand.command.reconfigureAudioStream.transport,
        nextCommand.command.reconfigureAudioStream.remoteSsrc);
}

void Engine::reconfigureVideoStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::ReconfigureVideoStream ||
        nextCommand.type == EngineCommand::Type::ReconfigureVideoStreamSecondary);

    if (nextCommand.type == EngineCommand::Type::ReconfigureVideoStream)
    {
        const auto& reconfigureVideoStream = nextCommand.command.reconfigureVideoStream;
        assert(reconfigureVideoStream.mixer);
        assert(reconfigureVideoStream.transport);

        logger::debug("Reconfigure video stream, mixer %s, endpointIdHash %lu, whitelist %c %u %u %u",
            "Engine",
            reconfigureVideoStream.mixer->getLoggableId().c_str(),
            reconfigureVideoStream.transport->getEndpointIdHash(),
            reconfigureVideoStream.ssrcWhitelist.enabled ? 't' : 'f',
            reconfigureVideoStream.ssrcWhitelist.numSsrcs,
            reconfigureVideoStream.ssrcWhitelist.ssrcs[0],
            reconfigureVideoStream.ssrcWhitelist.ssrcs[1]);

        auto mixer = reconfigureVideoStream.mixer;
        mixer->reconfigureVideoStream(reconfigureVideoStream.transport,
            reconfigureVideoStream.ssrcWhitelist,
            reconfigureVideoStream.simulcastStream);
    }
    else if (nextCommand.type == EngineCommand::Type::ReconfigureVideoStreamSecondary)
    {
        const auto& reconfigureVideoStreamSecondary = nextCommand.command.reconfigureVideoStreamSecondary;
        assert(reconfigureVideoStreamSecondary.mixer);
        assert(reconfigureVideoStreamSecondary.transport);

        logger::debug("Reconfigure video stream with secondary, mixer %s, endpointIdHash %lu, whitelist %c %u %u %u",
            "Engine",
            reconfigureVideoStreamSecondary.mixer->getLoggableId().c_str(),
            reconfigureVideoStreamSecondary.transport->getEndpointIdHash(),
            reconfigureVideoStreamSecondary.ssrcWhitelist.enabled ? 't' : 'f',
            reconfigureVideoStreamSecondary.ssrcWhitelist.numSsrcs,
            reconfigureVideoStreamSecondary.ssrcWhitelist.ssrcs[0],
            reconfigureVideoStreamSecondary.ssrcWhitelist.ssrcs[1]);

        auto mixer = reconfigureVideoStreamSecondary.mixer;
        mixer->reconfigureVideoStream(nextCommand.command.reconfigureVideoStreamSecondary.transport,
            reconfigureVideoStreamSecondary.ssrcWhitelist,
            reconfigureVideoStreamSecondary.simulcastStream,
            &reconfigureVideoStreamSecondary.secondarySimulcastStream);
    }
    else
    {
        assert(false);
    }
}

void Engine::addVideoPacketCache(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddVideoPacketCache);
    assert(nextCommand.command.addVideoPacketCache.mixer);
    assert(nextCommand.command.addVideoPacketCache.videoPacketCache);

    logger::debug("Add videoPacketCache, mixer %s, ssrc %u, endpointIdHash %lu",
        "Engine",
        nextCommand.command.addVideoPacketCache.mixer->getLoggableId().c_str(),
        nextCommand.command.addVideoPacketCache.ssrc,
        nextCommand.command.addVideoPacketCache.endpointIdHash);

    auto mixer = nextCommand.command.addVideoPacketCache.mixer;
    mixer->addVideoPacketCache(nextCommand.command.addVideoPacketCache.ssrc,
        nextCommand.command.addVideoPacketCache.endpointIdHash,
        nextCommand.command.addVideoPacketCache.videoPacketCache);
}

void Engine::processSctpControl(EngineCommand::Command& command)
{
    auto& sctpCommand = command.command.sctpControl;
    sctpCommand.mixer->handleSctpControl(sctpCommand.endpointIdHash, *command.packet);
}

void Engine::pinEndpoint(EngineCommand::Command& command)
{
    auto& pinCommand = command.command.pinEndpoint;
    pinCommand.mixer->pinEndpoint(pinCommand.endpointIdHash, pinCommand.pinnedEndpointIdHash);
}

void Engine::startRecording(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::StartRecording);
    assert(nextCommand.command.startRecording.mixer);
    assert(nextCommand.command.startRecording.recordingDesc);
    assert(nextCommand.command.startRecording.recordingStream);

    auto mixer = nextCommand.command.startRecording.mixer;
    mixer->recordingStart(nextCommand.command.startRecording.recordingStream,
        nextCommand.command.startRecording.recordingDesc);
}

void Engine::stopRecording(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::StopRecording);
    assert(nextCommand.command.stopRecording.mixer);
    assert(nextCommand.command.stopRecording.recordingDesc);
    assert(nextCommand.command.stopRecording.recordingStream);

    auto mixer = nextCommand.command.stopRecording.mixer;
    mixer->recordingStop(nextCommand.command.stopRecording.recordingStream,
        nextCommand.command.stopRecording.recordingDesc);

    EngineMessage::Message message = {EngineMessage::Type::RecordingStopped};
    message.command.recordingStopped.mixer = nextCommand.command.removeDataStream.mixer;
    message.command.recordingStopped.recordingDesc = nextCommand.command.stopRecording.recordingDesc;

    _messageListener->onMessage(std::move(message));
}

void Engine::addRecordingRtpPacketCache(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddRecordingRtpPacketCache);
    assert(nextCommand.command.addRecordingRtpPacketCache.mixer);
    assert(nextCommand.command.addRecordingRtpPacketCache.packetCache);

    logger::debug("Add RecordingRtpPacketCache, mixer %s, ssrc %u",
        "Engine",
        nextCommand.command.addRecordingRtpPacketCache.mixer->getLoggableId().c_str(),
        nextCommand.command.addRecordingRtpPacketCache.ssrc);

    auto mixer = nextCommand.command.addRecordingRtpPacketCache.mixer;
    mixer->addRecordingRtpPacketCache(nextCommand.command.addRecordingRtpPacketCache.ssrc,
        nextCommand.command.addRecordingRtpPacketCache.endpointIdHash,
        nextCommand.command.addRecordingRtpPacketCache.packetCache);
}

void Engine::addTransportToRecordingStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddTransportToRecordingStream);
    assert(nextCommand.command.addTransportToRecordingStream.mixer);
    assert(nextCommand.command.addTransportToRecordingStream.transport);
    assert(nextCommand.command.addTransportToRecordingStream.recUnackedPacketsTracker);

    auto mixer = nextCommand.command.addTransportToRecordingStream.mixer;
    mixer->addTransportToRecordingStream(nextCommand.command.addTransportToRecordingStream.streamIdHash,
        nextCommand.command.addTransportToRecordingStream.transport,
        nextCommand.command.addTransportToRecordingStream.recUnackedPacketsTracker);
}

void Engine::removeTransportFromRecordingStream(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::RemoveTransportFromRecordingStream);
    assert(nextCommand.command.removeTransportFromRecordingStream.mixer);
    assert(nextCommand.command.removeTransportFromRecordingStream.streamIdHash);
    assert(nextCommand.command.removeTransportFromRecordingStream.endpointIdHash);

    auto mixer = nextCommand.command.removeTransportFromRecordingStream.mixer;
    mixer->removeTransportFromRecordingStream(nextCommand.command.removeTransportFromRecordingStream.streamIdHash,
        nextCommand.command.removeTransportFromRecordingStream.endpointIdHash);
}

EngineStats::EngineStats Engine::getStats()
{
    EngineStats::EngineStats stats;
    _stats.read(stats);
    return stats;
}

void Engine::sendEndpointMessage(EngineCommand::Command& command)
{
    auto& endpointMessageCommand = command.command.endpointMessage;
    endpointMessageCommand.mixer->sendEndpointMessage(endpointMessageCommand.toEndpointIdHash,
        endpointMessageCommand.fromEndpointIdHash,
        endpointMessageCommand.message);
}

void Engine::addBarbell(EngineCommand::Command& nextCommand)
{
    assert(nextCommand.type == EngineCommand::Type::AddBarbell);
    assert(nextCommand.command.addBarbell.mixer);
    assert(nextCommand.command.addBarbell.engineBarbell);

    auto mixer = nextCommand.command.addBarbell.mixer;
    mixer->addBarbell(nextCommand.command.addBarbell.engineBarbell);
}

} // namespace bridge
