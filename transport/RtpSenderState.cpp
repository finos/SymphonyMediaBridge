#include "RtpSenderState.h"
#include "concurrency/MpmcPublish.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"

namespace transport
{
RtpSenderState::SendCounters operator-(RtpSenderState::SendCounters a, const RtpSenderState::SendCounters& b)
{
    a.octets -= b.octets;
    a.packets -= b.packets;
    a.sequenceNumber -= b.sequenceNumber;
    a.timestamp -= b.timestamp;
    return a;
}

RtpSenderState::RtpSenderState(uint32_t rtpFrequency, const config::Config& config)
    : payloadType(0xFF),
      _rtpSendTime(0),
      _senderReportSendTime(0),
      _senderReportNtp(0),
      _config(config),
      _scheduledSenderReport(0),
      _rtpFrequency(rtpFrequency)
{
}

void RtpSenderState::onRtpSent(uint64_t timestamp, memory::Packet& packet)
{
    _rtpSendTime = timestamp;
    auto* header = rtp::RtpHeader::fromPacket(packet);
    _rtpTimestampCorrelation.local = timestamp;
    _rtpTimestampCorrelation.rtp = header->timestamp;

    if (_sendCounters.octets == 0)
    {
        _sendCounters.sequenceNumber = header->sequenceNumber.get() - 1;
        _remoteReport.extendedSeqNoReceived = _sendCounters.sequenceNumber;
        _remoteReport.timestamp = timestamp;
        payloadType = header->payloadType;
    }
    _sendCounters.octets += packet.getLength() - header->headerLength();
    ++_sendCounters.packets;
    const int16_t sequenceDiff = header->sequenceNumber.get() - (_sendCounters.sequenceNumber & 0xFFFFu);

    if (sequenceDiff > 0)
    {
        _sendCounters.sequenceNumber += sequenceDiff;
    }

    if (_sendCounterSnapshot.timestamp == 0 ||
        utils::Time::diffGE(_sendCounterSnapshot.timestamp, timestamp, utils::Time::sec))
    {
        auto report = _sendCounters - _sendCounterSnapshot;
        _sendReport.write(report);
    }
}

void RtpSenderState::onRtcpSent(uint64_t timestamp, const rtp::RtcpHeader* header)
{
    if (header->packetType == rtp::RtcpPacketType::SENDER_REPORT)
    {
        auto* sr = rtp::RtcpSenderReport::fromPtr(header, header->size());
        _senderReportSendTime = timestamp;
        _senderReportNtp = sr->ntpSeconds << 16 | sr->ntpFractions >> 16;
        _scheduledSenderReport = timestamp + _config.rtcp.senderReport.resubmitInterval;
    }
}

uint32_t RtpSenderState::getRtpTimestamp(uint64_t timestamp) const
{
    const auto diff = static_cast<int64_t>(timestamp - _rtpTimestampCorrelation.local) / 1000;
    return _rtpTimestampCorrelation.rtp + static_cast<uint32_t>(_rtpFrequency * diff / 1000000llu);
}

void RtpSenderState::fillInReport(rtp::RtcpSenderReport& report, uint64_t timestamp, uint64_t wallClockNtp) const
{
    report.octetCount = _sendCounters.octets;
    report.packetCount = _sendCounters.packets;
    report.rtpTimestamp = getRtpTimestamp(timestamp);
    report.ntpSeconds = (wallClockNtp >> 32);
    report.ntpFractions = (wallClockNtp & 0xFFFFFFFFu);
}

// An RB may reference sender report from this ssrc but it may reference another SR by ntp timestamp.
// If the SR was not received, the ntp will be 0 and and SR is needed.
// We must reschedule our next SR regardless.
void RtpSenderState::onReceiverBlockReceived(uint64_t timestamp,
    uint32_t receiveTimeNtp32,
    const rtp::ReportBlock& report)
{
    RemoteCounters newReport;

    if (report.lastSR != 0 && report.delaySinceLastSR != 0)
    {
        newReport.rttNtp = receiveTimeNtp32 - report.lastSR - report.delaySinceLastSR;
        if (utils::Time::diffGT(timestamp, _scheduledSenderReport, _config.rtcp.senderReport.interval))
        {
            _scheduledSenderReport = timestamp + _config.rtcp.senderReport.interval;
        }
    }
    else
    {
        _scheduledSenderReport = timestamp + 20 * utils::Time::ms;
        newReport.rttNtp = 0;
    }

    if (report.loss.getCumulativeLoss() + 1 < _remoteReport.cumulativeLoss)
    {
        logger::debug("negative loss reported %u, %u",
            "SendState",
            report.loss.getCumulativeLoss(),
            _remoteReport.cumulativeLoss);
    }
    newReport.cumulativeLoss = report.loss.getCumulativeLoss();
    newReport.lossFraction = report.loss.getFractionLost();
    newReport.extendedSeqNoReceived = report.extendedSeqNoReceived;
    newReport.timestampNtp32 = receiveTimeNtp32;
    newReport.timestamp = timestamp;

    PacketCounters counters;
    counters.lostPackets = newReport.cumulativeLoss - _remoteReport.cumulativeLoss;
    counters.packets = newReport.extendedSeqNoReceived - _remoteReport.extendedSeqNoReceived;
    counters.period = timestamp - _remoteReport.timestamp;
    _counters.write(counters);

    ReportSummary s;
    s.extendedSeqNoReceived = newReport.extendedSeqNoReceived;
    s.sequenceNumberSent = _sendCounters.sequenceNumber;
    s.lostPackets = counters.lostPackets;
    s.lossFraction = newReport.lossFraction;
    s.packets = counters.packets;
    s.rttNtp = newReport.rttNtp;
    _summary.write(s);

    _remoteReport = newReport;
}

ReportSummary RtpSenderState::getSummary() const
{
    ReportSummary s;
    _summary.read(s);
    return s;
}

PacketCounters RtpSenderState::getCounters() const
{
    PacketCounters s;
    _counters.read(s);

    SendCounters sendReport;
    _sendReport.read(sendReport);

    s.octets = sendReport.octets;
    s.bitrateKbps = sendReport.octets * 8 * sendReport.timestamp / utils::Time::ms;
    s.packetsPerSecond = sendReport.packets * sendReport.timestamp / utils::Time::sec;

    return s;
}

uint32_t RtpSenderState::getRttNtp() const
{
    ReportSummary s;
    if (!_summary.read(s) || s.rttNtp == 0)
    {
        return ~0u;
    }
    return s.rttNtp;
}

// return ns until next sender report should be sent for this ssrc
int64_t RtpSenderState::timeToSenderReport(const uint64_t timestamp) const
{
    if (_scheduledSenderReport == 0)
    {
        return _sendCounters.packets > 4 ? 0 : _config.rtcp.senderReport.resubmitInterval;
    }

    return std::max(int64_t(0), static_cast<int64_t>(_scheduledSenderReport - timestamp));
}
} // namespace transport
