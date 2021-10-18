#include "RtpReceiveState.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
namespace transport
{

RtpReceiveState::ReceiveCounters operator-(RtpReceiveState::ReceiveCounters a,
    const RtpReceiveState::ReceiveCounters& b)
{
    a.octets -= b.octets;
    a.extendedSequenceNumber -= b.extendedSequenceNumber;
    a.packets -= b.packets;
    a.lostPackets -= b.lostPackets;
    a.timestamp -= b.timestamp;

    return a;
}

void RtpReceiveState::onRtpReceived(const memory::Packet& packet, uint64_t timestamp)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
    if (!rtpHeader)
    {
        return;
    }

    if (_receiveCounters.packets > 0)
    {
        const uint16_t prevSeq16 = _receiveCounters.extendedSequenceNumber & 0xFFFFu;
        const uint16_t currentSequenceNumber = rtpHeader->sequenceNumber;
        auto diff = static_cast<int16_t>(currentSequenceNumber - prevSeq16);
        if (diff > 0)
        {
            _receiveCounters.extendedSequenceNumber += diff;
        }

        if (diff > 1)
        {
            _receiveCounters.lostPackets += (diff - 1);
        }
        else if (diff < 0 && _receiveCounters.lostPackets > 0)
        {
            --_receiveCounters.lostPackets;
        }
    }
    else
    {
        payloadType = rtpHeader->payloadType;
        _receiveCounters.extendedSequenceNumber = rtpHeader->sequenceNumber;
        _receiveCounters.initialSequenceNumber = rtpHeader->sequenceNumber;
    }

    _activeAt = timestamp;
    ++_receiveCounters.packets;
    _receiveCounters.octets += packet.getLength() - rtpHeader->headerLength();
    _receiveCounters.timestamp = timestamp;

    if (_receiveCounters.packets < 10)
    {
        // report early reception of packets.
        _cumulativeReport.write(_receiveCounters);
    }

    if (utils::Time::diffGE(_previousCount.timestamp, timestamp, utils::Time::sec))
    {
        const auto delta = _receiveCounters - _previousCount;
        PacketCounters counters;
        counters.octets = delta.octets;
        counters.packets = delta.lostPackets + delta.packets;
        counters.lostPackets = delta.lostPackets;
        counters.period = delta.timestamp;
        _counters.write(counters);

        _previousCount = _receiveCounters;
        _cumulativeReport.write(_receiveCounters);
    }
}

void RtpReceiveState::onRtcpReceived(const rtp::RtcpHeader& report, uint64_t timestamp, uint64_t ntpWallclock)
{
    _activeAt = timestamp;

    auto senderReport = rtp::RtcpSenderReport::fromPtr(&report, report.size());
    if (senderReport)
    {
        _lastReceivedSenderReport._receiveTimeNtp = ntpWallclock;
        _lastReceivedSenderReport._ntp = senderReport->getNtp();
        _lastReceivedSenderReport._rtpTimestamp = senderReport->rtpTimestamp;
    }
}

void RtpReceiveState::fillInReportBlock(rtp::ReportBlock& reportBlock, uint64_t ntpNow) const
{
    reportBlock.extendedSeqNoReceived = _receiveCounters.extendedSequenceNumber;
    reportBlock.delaySinceLastSR = utils::Time::toNtp32(ntpNow - _lastReceivedSenderReport._receiveTimeNtp.load());
    reportBlock.interarrivalJitter = 0; // TODO measure jitter
    reportBlock.lastSR = utils::Time::toNtp32(_lastReceivedSenderReport._ntp);
    reportBlock.loss.setCumulativeLoss(_receiveCounters.lostPackets);
    reportBlock.loss.setFractionLost(_receiveCounters.packetLossRatio());
}

uint32_t RtpReceiveState::getExtendedSequenceNumber() const
{
    return _receiveCounters.extendedSequenceNumber;
}

PacketCounters RtpReceiveState::getCounters()
{
    PacketCounters counters;
    _counters.read(counters);
    counters.bitrateKbps = counters.octets * 8 * counters.period / utils::Time::ms;
    counters.packetsPerSecond = counters.getPacketsReceived() * counters.period / utils::Time::sec;
    return counters;
}

PacketCounters RtpReceiveState::getCumulativeCounters()
{
    ReceiveCounters report;
    _cumulativeReport.read(report);

    PacketCounters counters;
    counters.lostPackets = report.lostPackets;
    counters.packets = report.extendedSequenceNumber - report.initialSequenceNumber;
    counters.octets = report.octets;

    return counters;
}

} // namespace transport
