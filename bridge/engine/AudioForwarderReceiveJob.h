#pragma once

#include "jobmanager/Job.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>

namespace memory
{
class Packet;
}

namespace transport
{
class RtcTransport;
class DataReceiver;
} // namespace transport

namespace codec
{
class OpusDecoder;
}

namespace bridge
{

class EngineMixer;
class SsrcInboundContext;
class ActiveMediaList;

class AudioForwarderReceiveJob : public jobmanager::CountedJob
{
public:
    AudioForwarderReceiveJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioPacketAllocator,
        transport::RtcTransport* sender,
        EngineMixer& engineMixer,
        SsrcInboundContext& ssrcContext,
        ActiveMediaList& activeMediaList,
        const int32_t silenceThresholdLevel,
        const bool hasMixedAudioStreams,
        const uint32_t extendedSequenceNumber);

    ~AudioForwarderReceiveJob();

    void run() override;

private:
    void decodeOpus(const memory::Packet& opusPacket);
    void onPacketDecoded(const int32_t decodedFrames, const uint8_t* decodedData);

    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioPacketAllocator;
    EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    SsrcInboundContext& _ssrcContext;
    ActiveMediaList& _activeMediaList;
    int32_t _silenceThresholdLevel;
    bool _hasMixedAudioStreams;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
