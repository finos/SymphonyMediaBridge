#include "bridge/engine/DiscardReceivedVideoPacketJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SendPliJob.h"
#include "codec/Vp8Header.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"
#include "utils/Time.h"
#include <cstdio>

namespace bridge
{

DiscardReceivedVideoPacketJob::DiscardReceivedVideoPacketJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::SsrcInboundContext& ssrcContext)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _sender(sender),
      _ssrcContext(ssrcContext)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void DiscardReceivedVideoPacketJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        logger::debug("%s invalid rtp header. dropping",
            "DiscardReceivedVideoPacketJob",
            _sender->getLoggableId().c_str());
        return;
    }

    if (_ssrcContext.videoMissingPacketsTracker)
    {
        uint32_t extSeqno = 0;
        _ssrcContext.videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber, extSeqno);
    }
}

} // namespace bridge
