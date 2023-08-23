#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class Transport;
} // namespace transport

namespace bridge
{
class MixerManagerAsync;
class SsrcOutboundContext;
class SsrcInboundContext;
class EngineMixer;

class VideoForwarderRewriteAndSendJob : public jobmanager::CountedJob
{
public:
    VideoForwarderRewriteAndSendJob(SsrcOutboundContext& outboundContext,
        SsrcInboundContext& senderInboundContext,
        memory::UniquePacket packet,
        transport::Transport& transport,
        const uint32_t extendedSequenceNumber,
        MixerManagerAsync& mixerManager,
        size_t endpointIdHash,
        EngineMixer& mixer,
        uint64_t timestamp);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    SsrcInboundContext& _senderInboundContext;
    memory::UniquePacket _packet;
    transport::Transport& _transport;
    uint32_t _extendedSequenceNumber;
    MixerManagerAsync& _mixerManager;
    size_t _endpointIdHash;
    EngineMixer& _mixer;
    const uint64_t _timestamp;
};

} // namespace bridge
