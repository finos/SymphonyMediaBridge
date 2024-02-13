#pragma once

#include "bridge/engine/RtpForwarderReceiveBaseJob.h"
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

class ActiveMediaList;

class AudioForwarderReceiveJob : public RtpForwarderReceiveBaseJob
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
    int computeOpusAudioLevel(const memory::Packet& opusPacket);

    ActiveMediaList& _activeMediaList;
    const uint8_t _silenceThresholdLevel;
    const bool _hasMixedAudioStreams;
    const bool _needAudioLevel;
};

} // namespace bridge
