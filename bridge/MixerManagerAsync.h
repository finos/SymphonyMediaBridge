#pragma once
#include "memory/PacketPoolAllocator.h"
#include <string>

namespace jobmanager
{
class JobManager;
}

namespace codec
{
class OpusDecoder;
}

namespace utils
{
class Function;
}

namespace bridge
{

class PacketCache;
class Mixer;
class EngineMixer;
struct EngineAudioStream;
struct EngineVideoStream;
struct EngineDataStream;
struct EngineBarbell;
struct EngineRecordingStream;
struct RecordingDescription;

class MixerManagerAsync
{
public:
    virtual bool post(utils::Function&& task) = 0;

    virtual void allocateAudioBuffer(EngineMixer* mixer, uint32_t ssrc) = 0;
    virtual void audioStreamRemoved(EngineMixer* mixer, EngineAudioStream* audioStream) = 0;
    virtual void engineMixerRemoved(EngineMixer* mixer) = 0;
    virtual void freeVideoPacketCache(EngineMixer* mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void allocateVideoPacketCache(EngineMixer* mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void allocateRecordingRtpPacketCache(EngineMixer* mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void videoStreamRemoved(EngineMixer* engineMixer, EngineVideoStream* videoStream) = 0;
    virtual void sctpReceived(EngineMixer* mixer, memory::UniquePacket msgPacket, size_t endpointIdHash) = 0;
    virtual void dataStreamRemoved(EngineMixer* mixer, EngineDataStream* dataStream) = 0;
    virtual void freeRecordingRtpPacketCache(EngineMixer* mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void barbellRemoved(EngineMixer* mixer, EngineBarbell* barbell) = 0;
    virtual void recordingStreamRemoved(EngineMixer* mixer, EngineRecordingStream* recordingStream) = 0;
    virtual void removeRecordingTransport(EngineMixer* mixer, const char* streamId, size_t endpointIdHash) = 0;
    virtual void inboundSsrcContextRemoved(EngineMixer* mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder) = 0;
    virtual void mixerTimedOut(EngineMixer* mixer) = 0;
    virtual void engineRecordingStopped(EngineMixer* mixer, RecordingDescription* recordingDesc) = 0;
};

} // namespace bridge
