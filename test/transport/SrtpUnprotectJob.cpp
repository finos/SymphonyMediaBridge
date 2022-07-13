#include "test/transport/SrtpUnprotectJob.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

namespace transport
{

SrtpUnprotectJob::SrtpUnprotectJob(RtcTransport* sender, memory::UniquePacket packet, DecryptedPacketReceiver* receiver)
    : jobmanager::CountedJob(sender->getJobCounter()),
      _sender(sender),
      _packet(std::move(packet)),
      _receiver(receiver)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void SrtpUnprotectJob::run()
{
    if (rtp::isRtpPacket(*_packet) && _sender->unprotect(*_packet))
    {
        _receiver->onRtpPacketDecrypted(_sender, std::move(_packet));
    }
}

} // namespace transport
