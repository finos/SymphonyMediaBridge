#include "RtpReceiveState.h"
#include "config/Config.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
namespace transport
{

RtpReceiveState::ReceiveCounters operator-(RtpReceiveState::ReceiveCounters a,
    const RtpReceiveState::ReceiveCounters& b)
{
    a.octets -= b.octets;
    a.headerOctets -= b.headerOctets;
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
    _receiveCounters.headerOctets += rtpHeader->headerLength();
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
        counters.octets = delta.octets + delta.headerOctets;
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
        _lastReceivedSenderReport._receiveTime = timestamp;
        _scheduledReceiveReport = timestamp + _config.rtcp.receiveReport.delayAfterSR;
    }
}

void RtpReceiveState::fillInReportBlock(uint64_t timestamp, rtp::ReportBlock& reportBlock, uint64_t ntpNow)
{
    reportBlock.extendedSeqNoReceived = _receiveCounters.extendedSequenceNumber;
    reportBlock.interarrivalJitter = 0; // TODO measure jitter

    if (!_lastReceivedSenderReport.empty())
    {
        reportBlock.delaySinceLastSR = utils::Time::toNtp32(ntpNow - _lastReceivedSenderReport._receiveTimeNtp.load());
        reportBlock.lastSR = utils::Time::toNtp32(_lastReceivedSenderReport._ntp);
    }
    else
    {
        reportBlock.delaySinceLastSR = 0;
        reportBlock.lastSR = 0;
    }
    _scheduledReceiveReport = timestamp + _config.rtcp.receiveReport.idleInterval;

    reportBlock.loss.setCumulativeLoss(_receiveCounters.lostPackets);
    reportBlock.loss.setFractionLost(_receiveCounters.packetLossRatio());
}

uint32_t RtpReceiveState::getExtendedSequenceNumber() const
{
    return _receiveCounters.extendedSequenceNumber;
}

PacketCounters RtpReceiveState::getCounters() const
{
    PacketCounters counters;
    _counters.read(counters);
    counters.bitrateKbps = counters.octets * 8 * utils::Time::ms / std::max(utils::Time::ms * 10, counters.period);
    counters.packetsPerSecond =
        counters.getPacketsReceived() * utils::Time::sec / std::max(utils::Time::ms * 10, counters.period);
    return counters;
}

PacketCounters RtpReceiveState::getCumulativeCounters() const
{
    ReceiveCounters report;
    _cumulativeReport.read(report);

    PacketCounters counters;
    counters.lostPackets = report.lostPackets;
    counters.packets = report.extendedSequenceNumber - report.initialSequenceNumber;
    counters.octets = report.octets;

    return counters;
}

// return time left to send next receive report block
int64_t RtpReceiveState::timeToReceiveReport(uint64_t timestamp) const
{
    if (utils::Time::diffGE(_activeAt, timestamp, _config.rtcp.receiveReport.idleInterval))
    {
        return _config.rtcp.receiveReport.idleInterval;
    }

    if (_lastReceivedSenderReport.empty())
    {
        if (_scheduledReceiveReport == 0)
        {
            return (_receiveCounters.packets > 4 ? 0 : utils::Time::sec);
        }
    }

    return std::max(int64_t(0), static_cast<int64_t>(_scheduledReceiveReport - timestamp));
}

void RtpReceiveState::stop()
{
    _cumulativeReport.write(_receiveCounters);
    if (utils::Time::diffGE(_previousCount.timestamp, _receiveCounters.timestamp, utils::Time::ms * 250))
    {
        const auto delta = _receiveCounters - _previousCount;
        PacketCounters counters;
        counters.octets = delta.octets + delta.headerOctets;
        counters.packets = delta.lostPackets + delta.packets;
        counters.lostPackets = delta.lostPackets;
        counters.period = delta.timestamp;
        _counters.write(counters);
    }
}

} // namespace transport
