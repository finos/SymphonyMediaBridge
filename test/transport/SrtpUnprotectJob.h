#pragma once

#include "bridge/engine/SsrcInboundContext.h"
#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>

namespace bridge
{
class SsrcOutboundContext;
class EngineMixer;

} // namespace bridge
namespace memory
{
class Packet;
}

namespace jobmanager
{
class JobManager;
}

namespace rtp
{
struct RtpHeader;
}

namespace transport
{

class SrtpClient;
class RtcTransport;

class DecryptedPacketReceiver
{
public:
    virtual void onRtpPacketDecrypted(transport::RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        std::atomic_uint32_t& ownerCount) = 0;
};

class SrtpUnprotectJob : public jobmanager::CountedJob
{
public:
    SrtpUnprotectJob(RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        DecryptedPacketReceiver* receiver);
    void run() override;

private:
    RtcTransport* _sender;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _receiveAllocator;
    DecryptedPacketReceiver* _receiver;
};

} // namespace transport
