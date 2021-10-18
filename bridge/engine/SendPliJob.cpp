#include "bridge/engine/SendPliJob.h"
#include "rtp/RtcpFeedback.h"
#include "transport/Transport.h"

namespace bridge
{

SendPliJob::SendPliJob(const uint32_t fromSsrc,
    const uint32_t aboutSsrc,
    transport::Transport& transport,
    memory::PacketPoolAllocator& allocator)
    : CountedJob(transport.getJobCounter()),
      _aboutSsrc(aboutSsrc),
      _fromSsrc(fromSsrc),
      _transport(transport),
      _allocator(allocator)
{
}

void SendPliJob::run()
{
    auto* packet = memory::makePacket(_allocator);
    if (!packet)
    {
        return;
    }

    logger::info("%s sending PLI for %u from %u",
        "SendPliJob",
        _transport.getLoggableId().c_str(),
        _aboutSsrc,
        _fromSsrc);
    auto* feedback = rtp::createPLI(packet->get(), _fromSsrc, _aboutSsrc);
    packet->setLength(feedback->_header.size());
    _transport.protectAndSend(packet, _allocator);
}

} // namespace bridge
