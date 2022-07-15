#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{

class RtcTransport;

class DecryptedPacketReceiver
{
public:
    virtual void onRtpPacketDecrypted(transport::RtcTransport* sender, memory::UniquePacket packet) = 0;
};

class SrtpUnprotectJob : public jobmanager::CountedJob
{
public:
    SrtpUnprotectJob(RtcTransport* sender, memory::UniquePacket packet, DecryptedPacketReceiver* receiver);

    void run() override;

private:
    RtcTransport* _sender;
    memory::UniquePacket _packet;
    DecryptedPacketReceiver* _receiver;
};

} // namespace transport
