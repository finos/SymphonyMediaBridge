#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

class SsrcOutboundContext;

class RecordingRtpNackReceiveJob : public jobmanager::CountedJob
{
public:
    RecordingRtpNackReceiveJob(memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RecordingTransport* sender,
        SsrcOutboundContext& ssrcOutboundContext);

    ~RecordingRtpNackReceiveJob() override;

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    transport::RecordingTransport* _sender;
    SsrcOutboundContext& _ssrcOutboundContext;
};
} // namespace bridge
