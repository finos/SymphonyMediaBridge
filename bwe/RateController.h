#pragma once
#include "bwe/NetworkQueue.h"
#include "logger/Logger.h"
#include "memory/RandomAccessBacklog.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <array>
#include <cstdint>

namespace rtp
{
class ReportBlock;
}
namespace bwe
{
struct RateControllerConfig
{
    bool enabled = true;
    uint32_t ipOverhead = 20 + 14;
    uint32_t mtu = 1480;
    uint32_t maxPaddingSize = 1200;
    uint32_t bandwidthFloorKbps = 300;
    uint32_t bandwidthCeilingKbps = 9000;
    uint64_t minPadPinInterval = 80 * utils::Time::ms;
    uint32_t minPadSize = 50;
    uint32_t rtcpProbeCeiling = 600;
    uint32_t maxTargetQueue = 128000;
    uint32_t initialEstimateKbps = 1200;
    bool debugLog = false;
    uint64_t probeDuration = 700 * utils::Time::ms;
};

/*
Rate controlling is about modelling the network queue and propagation delay and use Receiver reports (RR) with RTT to
figure out what bandwidth best describes the observation. Typically, a packets travel time depends on three parameters:
- the size of the packet
- the bandwidth
- the size of the network queue at the time of insertion of packet
- the distance to the receiver

An RR contains information about which packet was last received and the time that has passed since the latest sender
report (SR). By tracking sent packets, we can calculate the receive rate in that window. We can also
calculate roughly the number of bytes still in transit. By modelling the network queue according to the expected
bandwidth, we assess if we were utilizing the link fully and the receive rate after the SR can guide us to the link
bandwidth. The probe receive rate is unreliable in that it can be highe rand lower than the actual through put. Also
there are multiple streams that send receiver reports at different points in time. The actual number of confirmed
received packets needs multiple receiver reports to establish. The confidence in the receive rate increases with the
number of packets confirmed and the time passed since the SR was received.

To get some useful data we need to probe the link by sending excess bandwidth over a short period of time.
This is done by interleaving the media packets with RTCP padding packets and Video RTX packets with old
sequence numbers. Both will be ignored by the receiver. To avoid packet loss this is done by filling the nework queue
and then maintain estimated bandwidth of the link. Either the bandwidth is correctly assessed and the network queue
remains after the probe, or it is higher and the receive rate is higher and the actual network queue is shorter than
predicted.
 */
class RateController
{
    struct PacketMetaData
    {
        uint64_t transmissionTime = 0;
        uint32_t ssrc = 0;
        uint32_t size = 0;
        uint32_t reportNtp = 0;
        uint32_t queueSize = 0;
        uint32_t sequenceNumber = 0;
        uint32_t lossCount = 0;
        uint32_t delaySinceSR = 0;
        bool received = false;
        enum Type : uint16_t
        {
            SR = 0,
            RR,
            RTP,
            RTCP_PADDING,
            SCTP
        } type = RTP;

        PacketMetaData() = default;
        ~PacketMetaData() = default;
        PacketMetaData(uint64_t timestamp,
            uint32_t streamSsrc,
            uint32_t packetSequenceNumber,
            uint32_t packetSize,
            Type packetType)
            : transmissionTime(timestamp),
              ssrc(streamSsrc),
              size(packetSize),
              reportNtp(0),
              queueSize(0),
              sequenceNumber(packetSequenceNumber),
              lossCount(0),
              delaySinceSR(0),
              received(false),
              type(packetType)
        {
        }
    };

    struct ReceiveBlockSample
    {
        uint32_t ssrc;
        uint32_t lossCount;

        ReceiveBlockSample() : ssrc(0), lossCount(~0u) {}
        explicit ReceiveBlockSample(uint32_t ssrc_) : ssrc(ssrc_), lossCount(0) {}
        bool empty() const { return ssrc == 0 && lossCount == ~0u; }
    };

public:
    explicit RateController(size_t instanceId, const RateControllerConfig& config);

    void onRtpSent(uint64_t timestamp, uint32_t ssrc, uint32_t sequenceNumber, uint16_t size);
    void onSenderReportSent(uint64_t timestamp, uint32_t ssrc, uint32_t reportNtp, uint16_t size);
    void onReportBlockReceived(uint32_t ssrc,
        uint32_t receivedSequenceNumber,
        uint32_t cumulativeLossCount,
        uint32_t reportNtp,
        uint32_t delaySinceSR);
    void onReportReceived(uint64_t timestamp, uint32_t count, const rtp::ReportBlock blocks[], uint32_t rttNtp);

    void onRtcpPaddingSent(uint64_t timestamp, uint32_t ssrc, uint16_t size);
    void onSctpSent(uint64_t timestamp, uint16_t size);

    uint32_t getPadding(uint64_t timestamp, uint16_t size, uint16_t& paddingSize) const;
    double getTargetRate() const;
    size_t getPacingBudget(uint64_t timestamp) const;
    void enableRtpProbing() { _canRtxPad = true; }

private:
    struct BacklogAnalysis
    {
        uint32_t transmitPeriod = 0;
        uint32_t receivedAfterSR = 0;
        uint32_t sentAfterSR = 0;
        uint32_t lossCount = 0;
        double lossRatio = 0;
        uint32_t delaySinceSR = 0;
        uint32_t packetsSent = 0;
        uint32_t packetsReceived = 0;
        uint32_t networkQueue = ~0u;
        bool probing = false;
        bool rtpProbe = true;
        const PacketMetaData* senderReportItem = nullptr;

        uint32_t getBitrateKbps() const;
        uint32_t getSendRateKbps() const;

        bool empty() const { return !senderReportItem; }
    };
    BacklogAnalysis bestReport(const BacklogAnalysis& probe1,
        const BacklogAnalysis& probe2,
        const uint32_t modelBandwidth) const;
    bool isGood(const BacklogAnalysis& probe, const uint32_t modelBandwidthKbps) const;

    void markReceivedPacket(uint32_t ssrc, uint32_t sequenceNumber);
    uint64_t calculateModelQueueTransmitPeriod();
    RateController::BacklogAnalysis analyzeProbe(const uint32_t probeEndIndex, const double modelBandwidthKbps) const;
    BacklogAnalysis analyzeBacklog(uint32_t reportNtp, double modelBandwidthKbps) const;

    double calculateSendRate(uint64_t timestamp) const;
    void dumpBacklog(uint32_t seqno, uint32_t ssrc);

    logger::LoggableId _logId;
    memory::RandomAccessBacklog<PacketMetaData, 512> _backlog;
    memory::RandomAccessBacklog<ReceiveBlockSample, 32> _receiveBlocks;

    struct Model
    {
        NetworkQueue queue;
        uint32_t targetQueue = 3000;
    } _model;

    double _drainMargin = 0;

    struct Probe
    {
        uint64_t start = 0;
        uint64_t interval = utils::Time::sec;
        uint64_t duration = 0;
        uint32_t count = 0;

        bool isProbing(uint64_t timestamp) const
        {
            return duration != 0 && utils::Time::diffLE(start, timestamp, duration);
        }
    } _probe;

    bool _canRtxPad = true;
    uint64_t _rtxSendTime = 0;

    uint32_t _minRttNtp;
    const RateControllerConfig& _config;
    uint64_t _lastLossBackoff = 0;
    struct
    {
        uint32_t ntp = 0;
        uint32_t delaySinceSR = 0;
        uint32_t packetsReceived = 0;
    } _lastProbe;
};
} // namespace bwe
