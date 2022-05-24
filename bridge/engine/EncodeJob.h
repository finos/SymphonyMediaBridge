#pragma once

#include "jobmanager/Job.h"
#include "memory/AudioPacketPoolAllocator.h"
#include <cstdint>

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
    EncodeJob(memory::UniqueAudioPacket packet,
        SsrcOutboundContext& outboundContext,
        transport::Transport& transport,
        const uint64_t rtpTimestamp);

    void run() override;

private:
    memory::UniqueAudioPacket _packet;
    SsrcOutboundContext& _outboundContext;
    transport::Transport& _transport;
    uint64_t _rtpTimestamp;
};

} // namespace bridge
