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
class PacketCache;

namespace EngineCommand
{

constexpr size_t endpointMessageMaxSize = 1024;

// Command definitions

struct AddMixer
{
    EngineMixer* _mixer;
};

struct RemoveMixer
{
    EngineMixer* _mixer;
};

struct AddRemoveAudioStream
{
    EngineMixer* _mixer;
    EngineAudioStream* _engineStream;
};

struct AddRemoveVideoStream
{
    EngineMixer* _mixer;
    EngineVideoStream* _engineStream;
};

struct ReconfigureAudioStream
{
    EngineMixer* _mixer;
    transport::RtcTransport* _transport;
    uint32_t _remoteSsrc;
};

struct ReconfigureVideoStream
{
    EngineMixer* _mixer;
    transport::RtcTransport* _transport;
    SimulcastStream _simulcastStream;
    SsrcWhitelist _ssrcWhitelist;
};

struct ReconfigureVideoStreamSecondary
{
    EngineMixer* _mixer;
    transport::RtcTransport* _transport;
    SimulcastStream _simulcastStream;
    SimulcastStream _secondarySimulcastStream;
    SsrcWhitelist _ssrcWhitelist;
};

struct AddRemoveDataStream
{
    EngineMixer* _mixer;
    EngineDataStream* _engineStream;
};

struct AddAudioBuffer
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    EngineMixer::AudioBuffer* _audioBuffer;
};

struct StartTransport
{
    EngineMixer* _mixer;
    transport::RtcTransport* _transport;
};

struct StartRecordingTransport
{
    EngineMixer* _mixer;
    transport::RecordingTransport* _transport;
};

struct AddVideoPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
    PacketCache* _videoPacketCache;
};

struct SctpControl
{
    EngineMixer* _mixer;
    size_t _endpointIdHash;
};

struct PinEndpoint
{
    EngineMixer* _mixer;
    size_t _endpointIdHash;
    size_t _pinnedEndpointIdHash;
};

struct EndpointMessage
{
    EngineMixer* _mixer;
    size_t _toEndpointIdHash;
    size_t _fromEndpointIdHash;
    char _message[endpointMessageMaxSize];
};

struct AddRemoveRecordingStream
{
    EngineMixer* _mixer;
    EngineRecordingStream* _recordingStream;
};

struct UpdateRecordingStreamModalities
{
    EngineMixer* _mixer;
    EngineRecordingStream* _recordingStream;
    bool _isAudioEnabled;
    bool _isVideoEnabled;
    bool _isScreenSharingEnabled;
};

struct StartStopRecording
{
    EngineMixer* _mixer;
    EngineRecordingStream* _recordingStream;
    RecordingDescription* _recordingDesc;
};

struct AddRecordingRtpPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
    PacketCache* _packetCache;
};

struct AddTransportToRecordingStream
{
    EngineMixer* _mixer;
    size_t _streamIdHash;
    transport::RecordingTransport* _transport;
    UnackedPacketsTracker* _recUnackedPacketsTracker;
};

struct RemoveTransportFromRecordingStream
{
    EngineMixer* _mixer;
    size_t _streamIdHash;
    size_t _endpointIdHash;
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
    AddRecordingUnackedPacketsTracker
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
};

struct Command
{
    Command() = default;
    Command(const Command&) = delete;
    Command(Type t) : _type(t) {}
    Command(Command&& rhs)
    {
        _type = rhs._type;
        _command = rhs._command; // TODO would need move assignment in the substructs ?
        _packet = std::move(rhs._packet);
    }

    Command& operator=(const Command&) = delete;

    Command& operator=(Command&& rhs)
    {
        _type = rhs._type;
        _command = rhs._command; // TODO would need move assignment in the substructs ?
        _packet = std::move(rhs._packet);
        return *this;
    }

    Type _type;
    CommandUnion _command;
    memory::UniquePacket _packet;
};

} // namespace EngineCommand

} // namespace bridge
