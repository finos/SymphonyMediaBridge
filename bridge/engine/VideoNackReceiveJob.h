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
        const uint32_t feedbackSsrc,
        const uint64_t timestamp,
        const uint64_t rtt);

    void run() override;

private:
    SsrcOutboundContext& _ssrcOutboundContext;
    transport::RtcTransport& _sender;
    PacketCache& _videoPacketCache;
    uint16_t _pid;
    uint16_t _blp;
    uint32_t _feedbackSsrc;
    uint64_t _timestamp;
    uint64_t _rtt;

    void sendIfCached(const uint16_t sequenceNumber);
};

} // namespace bridge
