#include "test/transport/SrtpUnprotectJob.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

namespace transport
{

SrtpUnprotectJob::SrtpUnprotectJob(RtcTransport* sender,
    memory::Packet* packet,
    memory::PacketPoolAllocator& receiveAllocator,
    DecryptedPacketReceiver* receiver)
    : jobmanager::CountedJob(sender->getJobCounter()),
      _sender(sender),
      _packet(packet),
      _receiveAllocator(receiveAllocator),
      _receiver(receiver)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

void SrtpUnprotectJob::run()
{
    if (rtp::isRtpPacket(*_packet))
    {
        if (!_sender->unprotect(_packet))
        {
            _receiveAllocator.free(_packet);
        }
        else
        {
            _receiver->onRtpPacketDecrypted(_sender, _packet, _receiveAllocator, getJobsCounter());
        }
    }
}

} // namespace transport
