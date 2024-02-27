#pragma once

#include "bridge/engine/RtpForwarderReceiveBaseJob.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"

namespace memory
{
class Packet;
}

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class EngineMixer;
class SsrcInboundContext;

class TelephoneEventForwardReceiveJob : public RtpForwarderReceiveBaseJob
{
public:
    TelephoneEventForwardReceiveJob(memory::UniquePacket packet,
        transport::RtcTransport* sender,
        EngineMixer& engineMixer,
        SsrcInboundContext& ssrcContext,
        uint32_t extendedSequenceNumber);

    void run() override;
};

} // namespace bridge
