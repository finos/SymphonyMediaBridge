#pragma once

#include "concurrency/MpmcPublish.h"
#include "transport/PacketCounters.h"
#include "utils/SocketAddress.h"
#include <algorithm>

namespace memory
{
class Packet;
} // namespace memory

namespace rtp
{
class ReportBlock;
struct RtcpHeader;
} // namespace rtp

namespace config
{
class Config;
}

namespace transport
{

class RtpReceiveState
{
public:
    explicit RtpReceiveState(const config::Config& config)
        : payloadType(0xFF),
          _config(config),
          _scheduledReceiveReport(0)
    {
    }

    void onRtpReceived(const memory::Packet& packet, uint64_t timestamp);
    void onRtcpReceived(const rtp::RtcpHeader& header, uint64_t timestamp, uint64_t ntpWallClock);
    void fillInReportBlock(uint64_t timestamp, rtp::ReportBlock& reportBlock, uint64_t ntpWallclock);

    uint64_t getLastActive() const { return _activeAt; }
    PacketCounters getCounters() const;
    PacketCounters getCumulativeCounters() const;
    uint32_t getExtendedSequenceNumber() const;
    uint64_t getSenderReportReceiveTimeNtp() const { return _lastReceivedSenderReport._receiveTimeNtp; }

    int64_t timeToReceiveReport(uint64_t timestamp) const;

    std::atomic_uint8_t payloadType;
    SocketAddress currentRtpSource;

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

        uint64_t octets;
        uint64_t headerOctets;
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
        SenderReportAggr() : _ntp(0), _rtpTimestamp(0), _receiveTimeNtp(0), _receiveTime(0) {}

        bool empty() const { return _ntp == 0 && _rtpTimestamp == 0 && _receiveTimeNtp == 0; }

        std::atomic<uint64_t> _ntp;
        uint32_t _rtpTimestamp;
        std::atomic<uint64_t> _receiveTimeNtp;
        uint64_t _receiveTime;
    };
    ReceiveCounters _receiveCounters;
    ReceiveCounters _previousCount;

    const config::Config& _config;
    SenderReportAggr _lastReceivedSenderReport;
    uint64_t _scheduledReceiveReport;
    std::atomic_uint64_t _activeAt;

    concurrency::MpmcPublish<PacketCounters, 4> _counters;
    concurrency::MpmcPublish<ReceiveCounters, 4> _cumulativeReport;
};

} // namespace transport
