#include "RtpSenderState.h"
#include "concurrency/MpmcPublish.h"
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

RtpSenderState::RtpSenderState(uint32_t rtpFrequency) : payloadType(0xFF), _sendTime(0), _rtpFrequency(rtpFrequency) {}

void RtpSenderState::onRtpSent(uint64_t timestamp, memory::Packet& packet)
{
    _sendTime = timestamp;
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

void RtpSenderState::onReceiverBlockReceived(uint64_t timestamp,
    uint32_t receiveTimeNtp32,
    const rtp::ReportBlock& report)
{
    RemoteCounters newReport;

    newReport.rttNtp = receiveTimeNtp32 - report.lastSR - report.delaySinceLastSR;
    if (report.loss.getCumulativeLoss() + 1 < _remoteReport.cumulativeLossCount)
    {
        logger::debug("negative loss reported %u, %u",
            "SendState",
            report.loss.getCumulativeLoss(),
            _remoteReport.cumulativeLossCount);
    }
    newReport.cumulativeLossCount = report.loss.getCumulativeLoss();
    newReport.lossFraction = report.loss.getFractionLost();
    newReport.extendedSeqNoReceived = report.extendedSeqNoReceived;
    newReport.timestampNtp32 = receiveTimeNtp32;

    PacketCounters counters;
    counters.lostPackets = newReport.cumulativeLossCount - _remoteReport.cumulativeLossCount;
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

PacketCounters RtpSenderState::getCounters()
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
    _summary.read(s);
    return s.rttNtp;
}

} // namespace transport
