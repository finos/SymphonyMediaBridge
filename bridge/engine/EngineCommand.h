#pragma once

#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcWhitelist.h"
#include <array>
#include <cstddef>
#include <utility>

namespace transport
{
class RtcTransport;
class RecordingTransport;
} // namespace transport

namespace bridge
{

struct EngineAudioStream;
struct EngineVideoStream;
struct EngineDataStream;
struct EngineBarbell;
class PacketCache;

namespace EngineCommand
{

// Command definitions

struct AddMixer
{
    EngineMixer* mixer;
};

struct RemoveMixer
{
    EngineMixer* mixer;
};

struct AddRemoveAudioStream
{
    EngineMixer* mixer;
    EngineAudioStream* engineStream;
};

struct AddRemoveVideoStream
{
    EngineMixer* mixer;
    EngineVideoStream* engineStream;
};

struct ReconfigureAudioStream
{
    EngineMixer* mixer;
    transport::RtcTransport* transport;
    uint32_t remoteSsrc;
};

struct ReconfigureVideoStream
{
    EngineMixer* mixer;
    transport::RtcTransport* transport;
    SimulcastStream simulcastStream;
    SsrcWhitelist ssrcWhitelist;
};

struct ReconfigureVideoStreamSecondary
{
    EngineMixer* mixer;
    transport::RtcTransport* transport;
    SimulcastStream simulcastStream;
    SimulcastStream secondarySimulcastStream;
    SsrcWhitelist ssrcWhitelist;
};

struct AddRemoveDataStream
{
    EngineMixer* mixer;
    EngineDataStream* engineStream;
};

struct AddAudioBuffer
{
    EngineMixer* mixer;
    uint32_t ssrc;
    EngineMixer::AudioBuffer* audioBuffer;
};

struct StartTransport
{
    EngineMixer* mixer;
    transport::RtcTransport* transport;
};

struct StartRecordingTransport
{
    EngineMixer* mixer;
    transport::RecordingTransport* transport;
};

struct AddVideoPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
    PacketCache* videoPacketCache;
};

struct SctpControl
{
    EngineMixer* mixer;
    size_t endpointIdHash;
};

struct PinEndpoint
{
    EngineMixer* mixer;
    size_t endpointIdHash;
    size_t pinnedEndpointIdHash;
};

struct EndpointMessage
{
    enum : size_t
    {
        MESSAGE_MAX_SIZE = 1024
    };
    EngineMixer* mixer;
    size_t toEndpointIdHash;
    size_t fromEndpointIdHash;
    char message[MESSAGE_MAX_SIZE];
};

struct AddRemoveRecordingStream
{
    EngineMixer* mixer;
    EngineRecordingStream* recordingStream;
};

struct UpdateRecordingStreamModalities
{
    EngineMixer* mixer;
    EngineRecordingStream* recordingStream;
    bool audioEnabled;
    bool videoEnabled;
    bool screenSharingEnabled;
};

struct StartStopRecording
{
    EngineMixer* mixer;
    EngineRecordingStream* recordingStream;
    RecordingDescription* recordingDesc;
};

struct AddRecordingRtpPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
    PacketCache* packetCache;
};

struct AddTransportToRecordingStream
{
    EngineMixer* mixer;
    size_t streamIdHash;
    transport::RecordingTransport* transport;
    UnackedPacketsTracker* recUnackedPacketsTracker;
};

struct RemoveTransportFromRecordingStream
{
    EngineMixer* mixer;
    size_t streamIdHash;
    size_t endpointIdHash;
};

struct AddBarbell
{
    EngineMixer* mixer;
    EngineBarbell* engineBarbell;
};

struct RemoveBarbell
{
    EngineMixer* mixer;
    size_t idHash;
};

// Add entry with same name as data struct here

enum class Type
{
    AddMixer,
    RemoveMixer,
    AddAudioStream,
    RemoveAudioStream,
    ReconfigureAudioStream,
    AddAudioBuffer,
    AddVideoStream,
    RemoveVideoStream,
    ReconfigureVideoStream,
    ReconfigureVideoStreamSecondary,
    AddDataStream,
    RemoveDataStream,
    StartTransport,
    StartRecordingTransport,
    AddVideoPacketCache,
    SctpControl,
    PinEndpoint,
    EndpointMessage,
    AddRecordingStream,
    RemoveRecordingStream,
    UpdateRecordingStreamModalities,
    StartRecording,
    StopRecording,
    AddRecordingRtpPacketCache,
    AddTransportToRecordingStream,
    RemoveTransportFromRecordingStream,
    AddRecordingUnackedPacketsTracker,
    AddBarbell,
    RemoveBarbell
};

// Add the data struct here

union CommandUnion
{
    AddMixer addMixer;
    RemoveMixer removeMixer;
    AddRemoveAudioStream addAudioStream;
    AddRemoveAudioStream removeAudioStream;
    ReconfigureAudioStream reconfigureAudioStream;
    AddAudioBuffer addAudioBuffer;
    AddRemoveVideoStream addVideoStream;
    AddRemoveVideoStream removeVideoStream;
    ReconfigureVideoStream reconfigureVideoStream;
    ReconfigureVideoStreamSecondary reconfigureVideoStreamSecondary;
    AddRemoveDataStream addDataStream;
    AddRemoveDataStream removeDataStream;
    StartTransport startTransport;
    StartRecordingTransport startRecordingTransport;
    AddVideoPacketCache addVideoPacketCache;
    SctpControl sctpControl;
    PinEndpoint pinEndpoint;
    EndpointMessage endpointMessage;
    AddRemoveRecordingStream addRecordingStream;
    AddRemoveRecordingStream removeRecordingStream;
    UpdateRecordingStreamModalities updateRecordingStreamModalities;
    StartStopRecording startRecording;
    StartStopRecording stopRecording;
    AddRecordingRtpPacketCache addRecordingRtpPacketCache;
    AddTransportToRecordingStream addTransportToRecordingStream;
    RemoveTransportFromRecordingStream removeTransportFromRecordingStream;
    AddBarbell addBarbell;
    RemoveBarbell removeBarbell;
};

struct Command
{
    Command() = default;
    Command(const Command&) = delete;
    Command(Type t) : type(t) {}
    Command(Command&& rhs)
    {
        type = rhs.type;
        command = rhs.command;
        packet = std::move(rhs.packet);
    }

    Command& operator=(const Command&) = delete;

    Command& operator=(Command&& rhs)
    {
        type = rhs.type;
        command = rhs.command;
        packet = std::move(rhs.packet);
        return *this;
    }

    Type type;
    CommandUnion command;
    memory::UniquePacket packet;
};

} // namespace EngineCommand

} // namespace bridge
