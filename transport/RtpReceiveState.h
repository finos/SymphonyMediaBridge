#pragma once

#include "concurrency/MpmcPublish.h"
#include "rtp/RtcpHeader.h"
#include "transport/PacketCounters.h"
#include <algorithm>
#include <chrono>

namespace transport
{

class RtpReceiveState
{
public:
    RtpReceiveState() : payloadType(0xFF) {}

    void onRtpReceived(const memory::Packet& packet, uint64_t timestamp);
    void onRtcpReceived(const rtp::RtcpHeader& header, uint64_t timestamp, uint64_t ntpWallClock);
    void fillInReportBlock(rtp::ReportBlock& reportBlock, uint64_t ntpWallclock) const;

    uint64_t getLastActive() const { return _activeAt; }
    PacketCounters getCounters();
    PacketCounters getCumulativeCounters();
    uint32_t getExtendedSequenceNumber() const;
    uint64_t getSenderReportReceiveTimeNtp() const { return _lastReceivedSenderReport._receiveTimeNtp; }

    std::atomic_uint8_t payloadType;

    struct ReceiveCounters
    {
        ReceiveCounters()
            : octets(0),
              extendedSequenceNumber(0),
              packets(0),
              lostPackets(0),
              timestamp(0),
              initialSequenceNumber(0)
        {
        }

        double packetLossRatio() const
        {
            return static_cast<double>(lostPackets) / std::max(1u, lostPackets + packets);
        }

        uint32_t octets;
        uint32_t extendedSequenceNumber;
        uint32_t packets;
        uint32_t lostPackets;
        uint64_t timestamp;
        uint32_t initialSequenceNumber;
        // TODO jitter avg
    };

private:
    struct SenderReportAggr
    {
        SenderReportAggr() : _ntp(0), _rtpTimestamp(0) {}

        std::atomic<uint64_t> _ntp;
        uint32_t _rtpTimestamp;
        std::atomic<uint64_t> _receiveTimeNtp;
    };
    ReceiveCounters _receiveCounters;

    ReceiveCounters _previousCount;

    SenderReportAggr _lastReceivedSenderReport;
    std::atomic_uint64_t _activeAt;

    concurrency::MpmcPublish<PacketCounters, 4> _counters;
    concurrency::MpmcPublish<ReceiveCounters, 4> _cumulativeReport;
};

} // namespace transport
