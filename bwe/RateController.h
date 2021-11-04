#pragma once
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
    uint32_t bandwidthCeilingKbps = 5000;
    uint64_t minPadPinInterval = 80 * utils::Time::ms;
    uint32_t minPadSize = 50;
};

/*
Rate controlling is about modelling the network queue and propagation delay and use Receiver reports (RR) with RTT to
figure out what bandwidth best describes the observation. Typically, a packets travel time depends on three parameters:
- the size of the packet
- the bandwidth
- the size of the network queue at the time of insertion of packet
- the distance to the receiver

An RR contains information about which packet was last received and the time that has passed since the latest sender
report (SR). By tracking sent packets, we can use that to calculate the receive rate in that window. We can also
calculate roughly the number of bytes still in transit. By modelling the network queue according to the bandwidth we
think it is, we can compare our model to the actual bytes in transit. It is not precise but if the actual queue is much
lower than expected, the bandwidth is likely higher and the opposite if the actual queue is longer than expected.

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
        uint32_t sequenceNumber = 0;
        uint32_t reportNtp = 0;
        uint32_t size = 0;
        uint32_t lossCount = 0;
        uint32_t queueSize = 0;
        bool received = false;
        enum Type : uint16_t
        {
            SR = 0,
            RR,
            RTP,
            RTCP_PADDING,
            RTP_PADDING
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
              sequenceNumber(packetSequenceNumber),
              reportNtp(0),
              size(packetSize),
              lossCount(0),
              queueSize(0),
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
        uint32_t reportNtp);
    void onReportReceived(uint64_t timestamp, uint32_t count, const rtp::ReportBlock blocks[], uint32_t rttNtp);

    void onRtcpPaddingSent(uint64_t timestamp, uint32_t ssrc, uint16_t size);
    void onRtpPaddingSent(uint64_t timestamp, uint32_t ssrc, uint32_t sequenceNumber, uint16_t size);

    uint32_t getPadding(uint64_t timestamp, uint16_t size, uint16_t& paddingSize) const;
    double getTargetRate() const { return (_config.enabled ? _model.bandwidthKbps : 0); }

private:
    void markReceivedPacket(uint32_t ssrc, uint32_t sequenceNumber);
    uint64_t calculateModelQueueTransmitPeriod();
    PacketMetaData* analyzeBacklog(uint32_t ssrc,
        uint32_t reportNtp,
        uint32_t receivedSequenceNumber,
        double modelBandwidthKbps,
        uint64_t& transmissionPeriod,
        uint32_t& receivedAfterSR,
        uint32_t& lossSinceSR,
        uint32_t& networkQueue,
        bool& probing);

    double calculateSendRate(uint64_t timestamp) const;

    logger::LoggableId _logId;
    memory::RandomAccessBacklog<PacketMetaData, 512> _backlog;
    memory::RandomAccessBacklog<ReceiveBlockSample, 32> _receiveBlocks;

    struct Model
    {
        double bandwidthKbps = 200.0;
        uint32_t queue = 0;
        uint32_t targetQueue = 3000;
        uint32_t networkQueue = 0;
        uint64_t lastTransmission = 0;

        void onPacketSent(uint64_t timestamp, uint16_t size)
        {
            queue -= std::min(queue,
                static_cast<uint32_t>(
                    utils::Time::diff(lastTransmission, timestamp) * bandwidthKbps / (8 * utils::Time::ms)));
            queue += size;
            lastTransmission = timestamp;
        }
    } _model;

    struct Probe
    {
        uint64_t start = 0;
        uint64_t interval = utils::Time::sec;
        uint64_t duration = 0;
        uint32_t initialNetworkQueue = 0;
        uint32_t count = 0;
    } _probe;

    uint32_t _canRtxPad = true;
    uint64_t _rtxSendTime = 0;

    uint32_t _minRttNtp;
    const RateControllerConfig& _config;
};
} // namespace bwe
