#pragma once

#include "concurrency/MpmcPublish.h"
#include "rtp/JitterTracker.h"
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
    explicit RtpReceiveState(const config::Config& config, uint32_t rtpFrequency)
        : payloadType(0xFF),
          _config(config),
          _scheduledReceiveReport(0),
          _jitterTracker(rtpFrequency > 0 ? rtpFrequency : 90000)
    {
    }

    void onRtpReceived(const memory::Packet& packet, uint64_t timestamp);
    void onRtcpReceived(const rtp::RtcpHeader& header, uint64_t timestamp, uint64_t ntpWallClock);
    void fillInReportBlock(uint64_t timestamp, rtp::ReportBlock& reportBlock, uint64_t ntpWallclock);
    void stop();

    uint64_t getLastActive() const { return _activeAt; }
    PacketCounters getCounters() const;
    PacketCounters getCumulativeCounters() const;
    uint32_t getExtendedSequenceNumber() const;
    uint32_t toExtendedSequenceNumber(uint16_t sequenceNumber) const;
    uint64_t getSenderReportReceiveTimeNtp() const { return _lastReceivedSenderReport._receiveTimeNtp; }

    int64_t timeToReceiveReport(uint64_t timestamp) const;
    void setRtpFrequency(uint32_t frequency);
    uint32_t getJitter() const;
    uint32_t getRtpFrequency() const;

    std::atomic_uint8_t payloadType;
    SocketAddress currentRtpSource;

    struct ReceiveCounters
    {
        ReceiveCounters()
            : octets(0),
              headerOctets(0),
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
    };

    // only for Transport thread context
    ReceiveCounters getCumulativeSnapshot() const { return _receiveCounters; }

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
    rtp::JitterTracker _jitterTracker;

    concurrency::MpmcPublish<PacketCounters, 4> _counters;
    concurrency::MpmcPublish<ReceiveCounters, 4> _cumulativeReport;
};

} // namespace transport
