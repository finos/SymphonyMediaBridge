#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class SsrcOutboundContext;
class PacketCache;

class VideoNackReceiveJob : public jobmanager::CountedJob
{
public:
    VideoNackReceiveJob(SsrcOutboundContext& ssrcOutboundContext,
        transport::RtcTransport& sender,
        PacketCache& videoPacketCache,
        const uint16_t pid,
        const uint16_t blp,
        const uint32_t feedbackSsrc);

    void run() override;

private:
    SsrcOutboundContext& _ssrcOutboundContext;
    transport::RtcTransport& _sender;
    PacketCache& _videoPacketCache;
    uint16_t _pid;
    uint16_t _blp;
    uint32_t _feedbackSsrc;

    void sendIfCached(const uint16_t sequenceNumber);
};

} // namespace bridge
