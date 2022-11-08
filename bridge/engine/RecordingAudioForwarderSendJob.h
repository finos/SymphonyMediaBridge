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

class SsrcOutboundContext;
class MixerManagerAsync;
class EngineMixer;

class RecordingAudioForwarderSendJob : public jobmanager::CountedJob
{
public:
    RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
        memory::UniquePacket packet,
        transport::RecordingTransport& transport,
        const uint32_t extendedSequenceNumber,
        MixerManagerAsync& mixerManager,
        size_t endpointIdHash,
        EngineMixer& mixer);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::UniquePacket _packet;
    transport::RecordingTransport& _transport;
    uint32_t _extendedSequenceNumber;
    MixerManagerAsync& _mixerManager;
    size_t _endpointIdHash;
    EngineMixer& _mixer;
};

} // namespace bridge
