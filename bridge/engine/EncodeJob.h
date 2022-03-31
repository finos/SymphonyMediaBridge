#pragma once

#include "jobmanager/Job.h"
#include "memory/AudioPacketPoolAllocator.h"

namespace transport
{
class Transport;
}

namespace bridge
{

class SsrcOutboundContext;

class EncodeJob : public jobmanager::CountedJob
{
public:
    EncodeJob(memory::AudioPacket* packet,
        memory::AudioPacketPoolAllocator& allocator,
        SsrcOutboundContext& outboundContext,
        transport::Transport& transport,
        uint64_t rtpTimestamp,
        uint8_t audioLevelExtensionId,
        uint8_t absSendTimeExtensionId);

    virtual ~EncodeJob();

    void run() override;

private:
    memory::AudioPacket* _packet;
    memory::AudioPacketPoolAllocator& _audioPacketPoolAllocator;
    SsrcOutboundContext& _outboundContext;
    transport::Transport& _transport;
    uint64_t _rtpTimestamp;
    uint8_t _audioLevelExtensionId;
    uint8_t _absSendTimeExtensionId;
};

} // namespace bridge
