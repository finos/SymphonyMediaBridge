#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{

class RtcTransport;

class DecryptedPacketReceiver
{
public:
    virtual void onRtpPacketDecrypted(transport::RtcTransport* sender,
        memory::PacketPtr packet,
        std::atomic_uint32_t& ownerCount) = 0;
};

class SrtpUnprotectJob : public jobmanager::CountedJob
{
public:
    SrtpUnprotectJob(RtcTransport* sender, memory::PacketPtr packet, DecryptedPacketReceiver* receiver);

    void run() override;

private:
    RtcTransport* _sender;
    memory::PacketPtr _packet;
    DecryptedPacketReceiver* _receiver;
};

} // namespace transport
