#pragma once

#include "bridge/engine/RtpForwarderReceiveBaseJob.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>

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

/** Receives, decrypts and pushes rtx packets as original video packet onto queues.
 * This makes them appear as received out of order, but they
 * can be stored in packet caches and forwarded to clients.
 */
class VideoForwarderRtxReceiveJob : public RtpForwarderReceiveBaseJob
{
public:
    VideoForwarderRtxReceiveJob(memory::UniquePacket packet,
        transport::RtcTransport* sender,
        bridge::EngineMixer& engineMixer,
        bridge::SsrcInboundContext& rtxSsrcContext,
        bridge::SsrcInboundContext& mainSsrcContext,
        uint32_t mainSsrc,
        uint32_t extendedSequenceNumber);

    void run() override;

private:
    bridge::SsrcInboundContext& _mainSsrcContext;
    uint32_t _mainSsrc;
};

} // namespace bridge
