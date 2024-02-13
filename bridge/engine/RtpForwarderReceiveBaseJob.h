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

class EngineMixer;
class SsrcInboundContext;

class RtpForwarderReceiveBaseJob : public jobmanager::CountedJob
{
protected:
    RtpForwarderReceiveBaseJob(memory::UniquePacket&& packet,
        transport::RtcTransport* sender,
        EngineMixer& engineMixer,
        SsrcInboundContext& ssrcContext,
        uint32_t extendedSequenceNumber);

    bool tryUnprotectRtpPacket(const char* logGroup);

protected:
    memory::UniquePacket _packet;
    EngineMixer& _engineMixer;
    transport::RtcTransport* _sender;
    SsrcInboundContext& _ssrcContext;
    const uint32_t _extendedSequenceNumber;
};

} // namespace bridge