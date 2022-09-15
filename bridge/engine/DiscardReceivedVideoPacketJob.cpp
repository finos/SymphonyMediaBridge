#include "bridge/engine/DiscardReceivedVideoPacketJob.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

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
