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
        transport::RtcTransport* sender,
        EngineMixer& engineMixer,
        SsrcInboundContext& ssrcContext,
        ActiveMediaList& activeMediaList,
        uint8_t silenceThresholdLevel,
        bool hasMixedAudioStreams,
        bool needAudioLevel,
        uint32_t extendedSequenceNumber);

    void run() override;

private:
    void decode(const memory::Packet& opusPacket, memory::AudioPacket& pcmPacket);
    bool unprotect(memory::Packet& opusPacket);
    int computeOpusAudioLevel(const memory::Packet& opusPacket);

    memory::UniquePacket _packet;
    EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    SsrcInboundContext& _ssrcContext;
    ActiveMediaList& _activeMediaList;
    const uint8_t _silenceThresholdLevel;
    const bool _hasMixedAudioStreams;
    const uint32_t _extendedSequenceNumber;
    const bool _needAudioLevel;
};

} // namespace bridge
