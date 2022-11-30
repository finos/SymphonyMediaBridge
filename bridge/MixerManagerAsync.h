#pragma once
#include "bridge/engine/EndpointId.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Function.h"
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

protected:
    virtual void allocateAudioBuffer(EngineMixer& mixer, uint32_t ssrc) = 0;
    virtual void audioStreamRemoved(EngineMixer& mixer, const EngineAudioStream& audioStream) = 0;
    virtual void engineMixerRemoved(EngineMixer& mixer) = 0;
    virtual void freeVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void allocateVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void allocateRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void videoStreamRemoved(EngineMixer& engineMixer, const EngineVideoStream& videoStream) = 0;
    virtual void sctpReceived(EngineMixer& mixer, memory::UniquePacket msgPacket, size_t endpointIdHash) = 0;
    virtual void dataStreamRemoved(EngineMixer& mixer, const EngineDataStream& dataStream) = 0;
    virtual void freeRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash) = 0;
    virtual void barbellRemoved(EngineMixer& mixer, const EngineBarbell& barbell) = 0;
    virtual void recordingStreamRemoved(EngineMixer& mixer, const EngineRecordingStream& recordingStream) = 0;
    virtual void removeRecordingTransport(EngineMixer& mixer, EndpointIdString streamId, size_t endpointIdHash) = 0;
    virtual void inboundSsrcContextRemoved(EngineMixer& mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder) = 0;
    virtual void mixerTimedOut(EngineMixer& mixer) = 0;
    virtual void engineRecordingStopped(EngineMixer& mixer, const RecordingDescription& recordingDesc) = 0;

public:
    bool asyncAllocateAudioBuffer(EngineMixer& mixer, uint32_t ssrc)
    {
        return post(utils::bind(&MixerManagerAsync::allocateAudioBuffer, this, std::ref(mixer), ssrc));
    }

    bool asyncAudioStreamRemoved(EngineMixer& mixer, const EngineAudioStream& audioStream);
    bool asyncEngineMixerRemoved(EngineMixer& mixer);
    bool asyncFreeVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash);
    bool asyncAllocateVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash);
    bool asyncAllocateRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash);
    bool asyncVideoStreamRemoved(EngineMixer& engineMixer, const EngineVideoStream& videoStream);
    bool asyncSctpReceived(EngineMixer& mixer, memory::UniquePacket& msgPacket, size_t endpointIdHash);
    bool asyncDataStreamRemoved(EngineMixer& mixer, const EngineDataStream& dataStream);
    bool asyncFreeRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash);
    bool asyncBarbellRemoved(EngineMixer& mixer, const EngineBarbell& barbell);
    bool asyncRecordingStreamRemoved(EngineMixer& mixer, const EngineRecordingStream& recordingStream);
    bool asyncRemoveRecordingTransport(EngineMixer& mixer, const char* streamId, size_t endpointIdHash);
    bool asyncInboundSsrcContextRemoved(EngineMixer& mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder);
    bool asyncMixerTimedOut(EngineMixer& mixer);
    bool asyncEngineRecordingStopped(EngineMixer& mixer, const RecordingDescription& recordingDesc);
};

} // namespace bridge
