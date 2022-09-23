#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class Transport;
} // namespace transport

namespace bridge
{
class EngineMessageListener;
class SsrcOutboundContext;
class SsrcInboundContext;
class EngineMixer;

class RecordingVideoForwarderSendJob : public jobmanager::CountedJob
{
public:
    RecordingVideoForwarderSendJob(SsrcOutboundContext& outboundContext,
        SsrcInboundContext& senderInboundContext,
        memory::UniquePacket packet,
        transport::Transport& transport,
        const uint32_t extendedSequenceNumber,
        EngineMessageListener& mixerManager,
        size_t endpointIdHash,
        EngineMixer& mixer);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    SsrcInboundContext& _senderInboundContext;
    memory::UniquePacket _packet;
    transport::Transport& _transport;
    uint32_t _extendedSequenceNumber;
    EngineMessageListener& _mixerManager;
    size_t _endpointIdHash;
    EngineMixer& _mixer;
};

} // namespace bridge
