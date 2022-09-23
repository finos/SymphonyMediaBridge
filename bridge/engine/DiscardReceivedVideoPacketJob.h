#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class SsrcInboundContext;

class DiscardReceivedVideoPacketJob : public jobmanager::CountedJob
{
public:
    DiscardReceivedVideoPacketJob(memory::UniquePacket packet,
        transport::RtcTransport* sender,
        bridge::SsrcInboundContext& ssrcContext,
        const uint32_t extendedSequenceNumber);

    void run() override;

private:
    memory::UniquePacket _packet;
    transport::RtcTransport* _sender;
    bridge::SsrcInboundContext& _ssrcContext;
    const uint32_t _extendedSequenceNumber;
};

} // namespace bridge
