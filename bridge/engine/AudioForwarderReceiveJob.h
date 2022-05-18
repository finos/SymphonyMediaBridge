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
        const uint8_t silenceThresholdLevel,
        const bool hasMixedAudioStreams,
        const uint32_t extendedSequenceNumber);

    void run() override;

private:
    void decodeOpus(const memory::Packet& opusPacket);
    void onPacketDecoded(const int32_t decodedFrames, const uint8_t* decodedData);
    void decodeG711(const memory::Packet& g711Packet);

    memory::UniquePacket _packet;
    EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    SsrcInboundContext& _ssrcContext;
    ActiveMediaList& _activeMediaList;
    const uint8_t _silenceThresholdLevel;
    const bool _hasMixedAudioStreams;
    const uint32_t _extendedSequenceNumber;
};

} // namespace bridge
