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
    EncodeJob(memory::AudioPacketPtr packet,
        SsrcOutboundContext& outboundContext,
        transport::Transport& transport,
        uint64_t rtpTimestamp,
        uint8_t audioLevelExtensionId,
        uint8_t absSendTimeExtensionId);

    void run() override;

private:
    memory::AudioPacketPtr _packet;
    SsrcOutboundContext& _outboundContext;
    transport::Transport& _transport;
    uint64_t _rtpTimestamp;
    uint8_t _audioLevelExtensionId;
    uint8_t _absSendTimeExtensionId;
};

} // namespace bridge
