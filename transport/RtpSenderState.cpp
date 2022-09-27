#include "RtpSenderState.h"
#include "concurrency/MpmcPublish.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"

namespace transport
{
RtpSenderState::SendCounters operator-(RtpSenderState::SendCounters a, const RtpSenderState::SendCounters& b)
{
    a.payloadOctets -= b.payloadOctets;
    a.rtpHeaderOctets -= b.rtpHeaderOctets;
    a.rtcpOctets -= b.rtcpOctets;
    a.packets -= b.packets;
    a.sequenceNumber -= b.sequenceNumber;
    a.timestamp -= b.timestamp;
    a.rtpTimestamp -= b.rtpTimestamp;
    return a;
}

RtpSenderState::RtpSenderState(uint32_t rtpFrequency, const config::Config& config)
    : payloadType(0xFF),
      _rtpSendTime(0),
      _senderReportSendTime(0),
      _senderReportNtp(0),
      _lossSinceSenderReport(0),
      _initialRtpTimestamp(0),
      _config(config),
      _scheduledSenderReport(0),
      _rtpFrequency(rtpFrequency)
{
}

void RtpSenderState::onRtpSent(uint64_t timestamp, memory::Packet& packet)
{
    _rtpSendTime = timestamp;
    auto* header = rtp::RtpHeader::fromPacket(packet);
    _sendCounters.timestamp = timestamp;
    _sendCounters.rtpTimestamp = header->timestamp;

    if (_sendCounters.payloadOctets == 0)
    {
        _sendCounters.sequenceNumber = header->sequenceNumber.get() - 1;
        _sendCounterSnapshot.timestamp = timestamp;
        _remoteReport.extendedSeqNoReceived = _sendCounters.sequenceNumber;
        _remoteReport.timestamp = timestamp;
        _initialRtpTimestamp = header->timestamp;
        payloadType = header->payloadType;
        ReportSummary summary;
        summary.initialRtpTimestamp = header->timestamp;
        summary.sequenceNumberSent = header->sequenceNumber;
        summary.packetsSent = 1;
        summary.rtpTimestamp = summary.initialRtpTimestamp;
        summary.rtpFrequency = _rtpFrequency;
        _summary.write(summary);
    }
    _sendCounters.payloadOctets += packet.getLength() - header->headerLength();
    _sendCounters.rtpHeaderOctets += header->headerLength();
    ++_sendCounters.packets;
    const int16_t sequenceDiff = header->sequenceNumber.get() - (_sendCounters.sequenceNumber & 0xFFFFu);

    if (sequenceDiff > 0)
    {
        _sendCounters.sequenceNumber += sequenceDiff;
    }

    if (utils::Time::diffGE(_sendCounterSnapshot.timestamp, timestamp, utils::Time::sec))
    {
        auto report = _sendCounters - _sendCounterSnapshot;
        _recentSent.write(report);
        _sendCounterSnapshot = _sendCounters;

        ReportSummary summary;
        _summary.read(summary);
        summary.sequenceNumberSent = _sendCounters.sequenceNumber;
        summary.packetsSent = _sendCounters.packets;
        summary.rtpTimestamp = _sendCounters.rtpTimestamp;
        summary.octets = _sendCounters.payloadOctets + _sendCounters.rtpHeaderOctets;
        _summary.write(summary);
    }
}

void RtpSenderState::onRtcpSent(uint64_t timestamp, const rtp::RtcpHeader* header, uint32_t packetSize)
{
    if (header->packetType == rtp::RtcpPacketType::SENDER_REPORT)
    {
        auto* sr = rtp::RtcpSenderReport::fromPtr(header, header->size());
        _senderReportSendTime = timestamp;
        _senderReportNtp = sr->ntpSeconds << 16 | sr->ntpFractions >> 16;
        _scheduledSenderReport = timestamp + _config.rtcp.senderReport.resubmitInterval;
        _lossSinceSenderReport = 0;
        _sendCounters.rtcpOctets += packetSize;
    }
}

uint32_t RtpSenderState::getRtpTimestamp(uint64_t timestamp) const
{
    const auto diff = static_cast<int64_t>(timestamp - _sendCounters.timestamp) / 1000;
    return _sendCounters.rtpTimestamp + static_cast<uint32_t>(_rtpFrequency * diff / 1000000llu);
}

void RtpSenderState::fillInReport(rtp::RtcpSenderReport& report, uint64_t timestamp, uint64_t wallClockNtp) const
{
    report.octetCount = _sendCounters.payloadOctets;
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
    _recentReceived.write(counters);
    _lossSinceSenderReport += counters.lostPackets;

    ReportSummary s;
    s.extendedSeqNoReceived = newReport.extendedSeqNoReceived;
    s.sequenceNumberSent = _sendCounters.sequenceNumber;
    s.lostPackets = newReport.cumulativeLoss;
    s.lossFraction = newReport.lossFraction;
    s.packetsSent = _sendCounters.packets;
    s.rttNtp = newReport.rttNtp;
    s.initialRtpTimestamp = _initialRtpTimestamp;
    s.rtpTimestamp = _sendCounters.rtpTimestamp;
    s.octets = _sendCounters.payloadOctets + _sendCounters.rtpHeaderOctets;
    s.rtpFrequency = _rtpFrequency;
    _summary.write(s);

    _remoteReport = newReport;
}

/**
 *  Cumulative send and receive stats. Except for lossFraction that is recent report.
 * */
ReportSummary RtpSenderState::getSummary() const
{
    ReportSummary s;
    _summary.read(s);
    return s;
}

/**
 * Recent receive and send counters.
 * Packets and lost packets are from recent receive report to calculate loss rate between receive reports.
 * packetsPerSecond, bitrateKbps, octets are actual sent in recent interval.
 */
PacketCounters RtpSenderState::getCounters() const
{
    PacketCounters s;
    _recentReceived.read(s);

    SendCounters sendReport;
    _recentSent.read(sendReport);

    s.octets = sendReport.payloadOctets + sendReport.rtcpOctets + sendReport.rtpHeaderOctets;
    s.bitrateKbps = s.octets * 8 * utils::Time::ms / std::max(utils::Time::ms * 10, sendReport.timestamp);
    s.packetsPerSecond = sendReport.packets * utils::Time::sec / std::max(utils::Time::ms * 10, sendReport.timestamp);

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

/**
 * return ns until next sender report should be sent for this ssrc
 * If we have loss reported for this SR, we need another SR soon to know if we still have loss on the link
 * as multiple report blocks may be referring to this SR.
 */
int64_t RtpSenderState::timeToSenderReport(const uint64_t timestamp) const
{
    if (_scheduledSenderReport == 0)
    {
        return _sendCounters.packets > 4 ? 0 : _config.rtcp.senderReport.resubmitInterval;
    }

    if (_lossSinceSenderReport > 0 && utils::Time::diffGE(_senderReportSendTime, timestamp, utils::Time::ms * 500))
    {
        return 0;
    }

    return std::max(int64_t(0), static_cast<int64_t>(_scheduledSenderReport - timestamp));
}

void RtpSenderState::stop()
{
    ReportSummary summary;
    _summary.read(summary);
    summary.sequenceNumberSent = _sendCounters.sequenceNumber;
    summary.packetsSent = _sendCounters.packets;
    summary.rtpTimestamp = _sendCounters.rtpTimestamp;
    summary.octets = _sendCounters.payloadOctets + _sendCounters.rtpHeaderOctets;
    summary.rtpFrequency = _rtpFrequency;
    _summary.write(summary);

    if (utils::Time::diffGE(_sendCounterSnapshot.timestamp, _sendCounters.timestamp, utils::Time::ms * 250))
    {
        auto report = _sendCounters - _sendCounterSnapshot;
        _recentSent.write(report);
    }
}
} // namespace transport
