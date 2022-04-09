#pragma once

#include "jobmanager/Job.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"

namespace memory
{
class Packet;
}

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class EngineMixer;
class SsrcInboundContext;
class ActiveMediaList;

class AudioForwarderReceiveJob : public jobmanager::CountedJob
{
public:
    AudioForwarderReceiveJob(memory::UniquePacket packet,
        memory::AudioPacketPoolAllocator& audioPacketAllocator,
        transport::RtcTransport* sender,
        EngineMixer& engineMixer,
        SsrcInboundContext& ssrcContext,
        ActiveMediaList& activeMediaList,
        const int32_t silenceThresholdLevel,
        const bool hasMixedAudioStreams,
        const uint32_t extendedSequenceNumber);

    void run() override;

private:
    void decodeOpus(const memory::Packet& opusPacket);
    void onPacketDecoded(const int32_t decodedFrames, const uint8_t* decodedData);

    memory::UniquePacket _packet;
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
