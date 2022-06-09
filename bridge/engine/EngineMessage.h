#pragma once

#include "memory/PacketPoolAllocator.h"
#include <cstddef>
#include <cstdint>

namespace codec
{
class OpusDecoder;
} // namespace codec

namespace bridge
{

class EngineMixer;
struct EngineAudioStream;
struct EngineVideoStream;
struct EngineDataStream;
struct EngineBarbell;
struct EngineRecordingStream;
struct RecordingDescription;

namespace EngineMessage
{

struct MixerRemoved
{
    EngineMixer* _mixer;
};

struct AllocateAudioBuffer
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
};

struct AudioStreamRemoved
{
    EngineMixer* _mixer;
    EngineAudioStream* _engineStream;
};

struct VideoStreamRemoved
{
    EngineMixer* _mixer;
    EngineVideoStream* _engineStream;
};

struct RecordingStreamRemoved
{
    EngineMixer* _mixer;
    EngineRecordingStream* _engineStream;
};

struct DataStreamRemoved
{
    EngineMixer* _mixer;
    EngineDataStream* _engineStream;
};

struct MixerTimedOut
{
    EngineMixer* _mixer;
};

struct SsrcInboundRemoved
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    codec::OpusDecoder* _opusDecoder;
};

struct AllocateVideoPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
};

struct FreeVideoPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
};

struct SctpMessage
{
    EngineMixer* _mixer;
    size_t _endpointIdHash;
};

struct RecordingStopperMessage
{
    EngineMixer* _mixer;
    RecordingDescription* _recordingDesc;
};

struct AllocateRecordingRtpPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
};

struct FreeRecordingRtpPacketCache
{
    EngineMixer* _mixer;
    uint32_t _ssrc;
    size_t _endpointIdHash;
};

struct RemoveRecordingTransport
{
    EngineMixer* _mixer;
    const char* _streamId;
    size_t _endpointIdHash;
};

struct EngineBarbellRemoved
{
    EngineMixer* mixer;
    EngineBarbell* barbell;
};

enum class Type
{
    MixerRemoved,
    AllocateAudioBuffer,
    AudioStreamRemoved,
    VideoStreamRemoved,
    RecordingStreamRemoved,
    DataStreamRemoved,
    MixerTimedOut,
    AllocateVideoPacketCache,
    SctpMessage,
    InboundSsrcRemoved,
    FreeVideoPacketCache,
    RecordingStopped,
    AllocateRecordingRtpPacketCache,
    FreeRecordingRtpPacketCache,
    RemoveRecordingTransport,
    BarbellRemoved
};

union MessageUnion
{
    MixerRemoved mixerRemoved;
    AllocateAudioBuffer allocateAudioBuffer;
    AudioStreamRemoved audioStreamRemoved;
    VideoStreamRemoved videoStreamRemoved;
    RecordingStreamRemoved recordingStreamRemoved;
    DataStreamRemoved dataStreamRemoved;
    MixerTimedOut mixerTimedOut;
    AllocateVideoPacketCache allocateVideoPacketCache;
    FreeVideoPacketCache freeVideoPacketCache;
    SctpMessage sctpMessage;
    SsrcInboundRemoved ssrcInboundRemoved;
    RecordingStopperMessage recordingStopped;
    AllocateRecordingRtpPacketCache allocateRecordingRtpPacketCache;
    FreeRecordingRtpPacketCache freeRecordingRtpPacketCache;
    RemoveRecordingTransport removeRecordingTransport;
    EngineBarbellRemoved barbellRemoved;
};

struct Message
{
    Message() = default;
    Message(const Message&) = delete;
    Message(Type t) : _type(t) {}
    Message(Message&& rhs)
    {
        _type = rhs._type;
        _command = rhs._command;
        _packet = std::move(rhs._packet);
    }

    Message& operator=(const Message&) = delete;

    Message& operator=(Message&& rhs)
    {
        _type = rhs._type;
        _command = rhs._command;
        _packet = std::move(rhs._packet);
        return *this;
    }

    Type _type;
    MessageUnion _command;
    memory::UniquePacket _packet;
}; // namespace bridge

} // namespace EngineMessage

} // namespace bridge
