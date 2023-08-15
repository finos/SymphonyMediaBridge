#include "bridge/engine/DiscardReceivedVideoPacketJob.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"

namespace bridge
{

DiscardReceivedVideoPacketJob::DiscardReceivedVideoPacketJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::SsrcInboundContext& ssrcContext,
    uint32_t extendedSequenceNumber,
    uint64_t timestamp)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _extendedSequenceNumber(extendedSequenceNumber),
      _timestamp(timestamp)
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

    const bool noPacketsProcessedYet =
        (_ssrcContext.packetsProcessed == 0 && _ssrcContext.lastReceivedExtendedSequenceNumber == 0 &&
            _ssrcContext.lastUnprotectedExtendedSequenceNumber == 0);

    if (noPacketsProcessedYet && (_extendedSequenceNumber >> 16) == 0)
    {
        _sender->unprotect(*_packet); // make sure srtp sees one packet with ROC=0
        _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    }

    _ssrcContext.onRtpPacketReceived(_timestamp);
    _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
    if (_ssrcContext.videoMissingPacketsTracker)
    {
        uint32_t extSeqno = 0;
        _ssrcContext.videoMissingPacketsTracker->onPacketArrived(rtpHeader->sequenceNumber, extSeqno);
    }
}

} // namespace bridge
