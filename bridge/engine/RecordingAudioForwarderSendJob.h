#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

namespace memory
{
class Packet;
} // namespace memory

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

class SsrcInboundContext;
class SsrcOutboundContext;
class MixerManagerAsync;
class EngineMixer;

class RecordingAudioForwarderSendJob : public jobmanager::CountedJob
{
public:
    RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
        memory::UniquePacket packet,
        transport::RecordingTransport& transport,
        const SsrcInboundContext& ssrcSenderInboundContext,
        const uint32_t extendedSequenceNumber,
        MixerManagerAsync& mixerManager,
        size_t endpointIdHash,
        EngineMixer& mixer,
        uint64_t timestamp);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::UniquePacket _packet;
    transport::RecordingTransport& _transport;
    const SsrcInboundContext& _ssrcSenderInboundContext;
    const uint32_t _extendedSequenceNumber;
    MixerManagerAsync& _mixerManager;
    const size_t _endpointIdHash;
    EngineMixer& _mixer;
    const uint64_t _timestamp;
};

} // namespace bridge
