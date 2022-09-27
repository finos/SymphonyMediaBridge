#pragma once

#include "concurrency/MpmcPublish.h"
#include "transport/PacketCounters.h"

namespace memory
{
class Packet;
} // namespace memory
namespace rtp
{
class ReportBlock;
class RtcpSenderReport;
struct RtcpHeader;
} // namespace rtp

namespace config
{
class Config;
}

namespace transport
{
struct ReportSummary
{
    bool empty() const { return packetsSent == 0 && sequenceNumberSent == 0; }
    uint64_t getRtt() const { return (static_cast<uint64_t>(rttNtp) * utils::Time::sec) >> 16; }

    uint32_t lostPackets = 0;
    double lossFraction = 0;
    uint32_t extendedSeqNoReceived = 0;
    uint32_t sequenceNumberSent = 0;
    uint32_t rttNtp = 0;
    uint32_t packetsSent = 0;
    uint32_t initialRtpTimestamp = 0;
    uint32_t rtpTimestamp = 0;
    uint64_t octets = 0;
    uint32_t rtpFrequency;
};

class RtpSenderState
{
public:
    explicit RtpSenderState(uint32_t rtpFrequency, const config::Config& config);

    // Transport interface
    void onRtpSent(uint64_t timestamp, memory::Packet& packet);
    void onReceiverBlockReceived(uint64_t timestamp, uint32_t wallClockNtp32, const rtp::ReportBlock& report);
    void onRtcpSent(uint64_t timestamp, const rtp::RtcpHeader* report, uint32_t packetSize);
    int64_t timeToSenderReport(uint64_t timestamp) const;

    uint64_t getLastSendTime() const { return _rtpSendTime; }
    uint32_t getSentSequenceNumber() const { return _sendCounters.sequenceNumber; }
    uint32_t getSentPacketsCount() const { return _sendCounters.packets; }
    void fillInReport(rtp::RtcpSenderReport& report, uint64_t timestamp, uint64_t wallClockNtp) const;

    void setRtpFrequency(uint32_t rtpFrequency);
    void stop();

    // thread safe interface
    ReportSummary getSummary() const;
    PacketCounters getCounters() const;
    uint32_t getRttNtp() const;
    std::atomic_uint8_t payloadType;

    struct SendCounters
    {
        uint64_t payloadOctets = 0;
        uint64_t timestamp = 0;
        uint64_t rtcpOctets = 0;
        uint64_t rtpHeaderOctets = 0;
        uint32_t packets = 0;
        uint32_t sequenceNumber = 0;
        uint32_t rtpTimestamp = 0;
    };

    uint32_t getRtpTimestamp(uint64_t timestamp) const;

private:
    struct RemoteCounters
    {
        RemoteCounters()
            : cumulativeLoss(0),
              rttNtp(0),
              lossFraction(0),
              extendedSeqNoReceived(0),
              timestampNtp32(0),
              timestamp(0)
        {
        }

        uint32_t cumulativeLoss;
        uint32_t rttNtp;
        double lossFraction;
        uint32_t extendedSeqNoReceived;
        uint32_t timestampNtp32;
        uint64_t timestamp;
    };

    SendCounters _sendCounters;
    SendCounters _sendCounterSnapshot;

    std::atomic_uint64_t _rtpSendTime;
    uint64_t _senderReportSendTime;
    uint32_t _senderReportNtp;
    uint32_t _lossSinceSenderReport;
    uint32_t _initialRtpTimestamp;

    const config::Config& _config;
    uint64_t _scheduledSenderReport;

    RemoteCounters _remoteReport;

    uint32_t _rtpFrequency;
    concurrency::MpmcPublish<PacketCounters, 4> _recentReceived;
    concurrency::MpmcPublish<ReportSummary, 4> _summary;
    concurrency::MpmcPublish<SendCounters, 4> _recentSent;
};

} // namespace transport
