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
    EngineMixer* mixer;
};

struct AllocateAudioBuffer
{
    EngineMixer* mixer;
    uint32_t ssrc;
};

struct AudioStreamRemoved
{
    EngineMixer* mixer;
    EngineAudioStream* engineStream;
};

struct VideoStreamRemoved
{
    EngineMixer* mixer;
    EngineVideoStream* engineStream;
};

struct RecordingStreamRemoved
{
    EngineMixer* mixer;
    EngineRecordingStream* engineStream;
};

struct DataStreamRemoved
{
    EngineMixer* mixer;
    EngineDataStream* engineStream;
};

struct MixerTimedOut
{
    EngineMixer* mixer;
};

struct SsrcInboundRemoved
{
    EngineMixer* mixer;
    uint32_t ssrc;
    codec::OpusDecoder* opusDecoder;
};

struct AllocateVideoPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
};

struct FreeVideoPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
};

struct SctpMessage
{
    EngineMixer* mixer;
    size_t endpointIdHash;
};

struct RecordingStopperMessage
{
    EngineMixer* mixer;
    RecordingDescription* recordingDesc;
};

struct AllocateRecordingRtpPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
};

struct FreeRecordingRtpPacketCache
{
    EngineMixer* mixer;
    uint32_t ssrc;
    size_t endpointIdHash;
};

struct RemoveRecordingTransport
{
    EngineMixer* mixer;
    const char* streamId;
    size_t endpointIdHash;
};

struct EngineBarbellMessage
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
    BarbellRemoved,
    BarbellIdle
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
    EngineBarbellMessage barbellMessage;
};

struct Message
{
    Message() = default;
    Message(const Message&) = delete;
    Message(Type t) : type(t) {}
    Message(Message&& rhs)
    {
        type = rhs.type;
        command = rhs.command;
        packet = std::move(rhs.packet);
    }

    Message& operator=(const Message&) = delete;

    Message& operator=(Message&& rhs)
    {
        type = rhs.type;
        command = rhs.command;
        packet = std::move(rhs.packet);
        return *this;
    }

    Type type;
    MessageUnion command;
    memory::UniquePacket packet;
}; // namespace bridge

} // namespace EngineMessage

} // namespace bridge
