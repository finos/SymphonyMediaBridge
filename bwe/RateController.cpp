#include "bwe/RateController.h"
#include "logger/Logger.h"
#include "math/helpers.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/CheckedCast.h"
#include "utils/StdExtensions.h"
#include "utils/Time.h"
#include <algorithm>
#include <cmath>

#define RCTL_LOG(fmt, ...)                                                                                             \
    if (_config.debugLog)                                                                                              \
    {                                                                                                                  \
        logger::debug(fmt, ##__VA_ARGS__);                                                                             \
    }

namespace
{
const uint32_t ntp32Second = 0x10000u;
uint32_t calculateTargetQueue(double bandwidthKbps, uint32_t minRttNtp, const bwe::RateControllerConfig& config)
{
    // 170ms queue at 300kbps then decay to 50ms. Add for min RTT/8 ms to allow for queue on long haul
    const uint32_t targetQueue =
        bandwidthKbps * (40 + 51000 / std::max(200.0, bandwidthKbps) + minRttNtp * 125 / ntp32Second) / 8;
    return math::clamp(targetQueue, 4 * config.mtu, config.maxTargetQueue);
}

} // namespace
namespace bwe
{

uint32_t RateController::BacklogAnalysis::getBitrateKbps() const
{
    if (delaySinceSR == 0)
    {
        return 0;
    }

    const auto bitrate = static_cast<uint64_t>(receivedAfterSR) * ntp32Second / (125 * delaySinceSR);
    if (bitrate >= std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    return bitrate;
}

uint32_t RateController::BacklogAnalysis::getSendRateKbps() const
{
    if (transmitPeriod == 0)
    {
        return 0;
    }
    const auto bitrate = static_cast<uint64_t>(receivedAfterSR) * utils::Time::ms * 8 / transmitPeriod;
    if (bitrate >= std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    return bitrate;
}

RateController::RateController(size_t instanceId, const RateControllerConfig& config)
    : _logId("RateCtrl", instanceId),
      _minRttNtp(~0u),
      _config(config)
{
    _model.queue.setBandwidth(config.initialEstimateKbps);
    _model.targetQueue = calculateTargetQueue(_model.queue.getBandwidth(), 1, config);
}

void RateController::onRtpSent(uint64_t timestamp, uint32_t ssrc, uint32_t sequenceNumber, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    if (_minRttNtp != ~0u)
    {
        _minRttNtp += 2;
    }

    _model.queue.onPacketSent(timestamp, size + _config.ipOverhead);
    _backlog.emplace_front(timestamp, ssrc, sequenceNumber, size + _config.ipOverhead, PacketMetaData::RTP);
}

void RateController::onSenderReportSent(uint64_t timestamp, uint32_t ssrc, uint32_t reportNtp, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    _model.queue.onPacketSent(timestamp, size + _config.ipOverhead);

    PacketMetaData metaPacket(timestamp, ssrc, 0, size, PacketMetaData::SR);
    metaPacket.reportNtp = reportNtp;
    metaPacket.queueSize = _model.queue.size();
    _backlog.push_front(metaPacket);

    const auto timeToProbe = _probe.start == 0 || utils::Time::diffGE(_probe.start, timestamp, _probe.interval);

    if (_model.queue.size() < _model.targetQueue && ((_probe.count < 5 && _probe.duration == 0) || timeToProbe))
    {
        _probe.start = timestamp;
        _probe.duration = _config.probeDuration;
        ++_probe.count;
        RCTL_LOG("starting probe %" PRIu64 "ms target Q %uB, mQ %uB, ntp %x",
            _logId.c_str(),
            _probe.duration / utils::Time::ms,
            _model.targetQueue * 2,
            _model.queue.size(),
            reportNtp);
    }
}

/**
 * Marks received packets in the backlog accoring to this ssrc and sequence number
 *
 * Improvement could be to mark the packets lost according to loss count. Sort the packets in heap and mark those with
 * largest size between the received seqno and the last received seqno. This can be used to more accurately decide how
 * many bytes were actually received after SR. At the moment we pick the largest packet and multiply with the loss count
 * although it may be other packet sequences that are lost. Also we should take into account sequence number gaps in
 * transmission due to dropping packets in pacing queue.
 */
void RateController::onReportBlockReceived(uint32_t ssrc,
    uint32_t receivedSequenceNumber,
    uint32_t cumulativeLossCount,
    uint32_t reportNtp,
    uint32_t delaySinceSR)
{
    if (!_config.enabled)
    {
        return;
    }

    ReceiveBlockSample* sample = nullptr;
    for (auto& sampleBlock : _receiveBlocks)
    {
        if (sampleBlock.ssrc == ssrc)
        {
            sample = &sampleBlock;
            break;
        }
    }
    if (!sample)
    {
        _receiveBlocks.emplace_front(ssrc);
        sample = &_receiveBlocks.front();
    }

    uint32_t lossCount = cumulativeLossCount - std::min(cumulativeLossCount, sample->lossCount);
    sample->lossCount = cumulativeLossCount;

    uint32_t receivedPackets = 0;
    for (auto& item : _backlog)
    {
        if (item.ssrc == ssrc && item.type == PacketMetaData::RTP)
        {
            if (item.received)
            {
                break; // part of previous report on this ssrc
                // at this point we could walk forward and mark packets to sacrifice as loss according to lossCount.
                // That would give us ability to assess bandwidth also when some loss is present
            }
            else if (item.sequenceNumber == receivedSequenceNumber)
            {
                item.received = true;
                item.lossCount = lossCount;
                item.reportNtp = reportNtp;
                item.delaySinceSR = delaySinceSR;
                ++receivedPackets;
                if (lossCount > 0)
                {
                    RCTL_LOG("marking %u, %u with loss %u", _logId.c_str(), ssrc, item.sequenceNumber, lossCount);
                }
            }
            else if (receivedPackets > 0)
            {
                item.received = true;
                ++receivedPackets;
            }
        }
    }
    if (lossCount > 0)
    {
        // dumpBacklog(receivedSequenceNumber, ssrc);
    }
}

namespace
{

/** Verify that network queue is above initial queue length through out the probe.
 * This means we have applied pressure during the probe.
 */
template <typename T>
bool isProbe(const T& backlog, double bandwidthKbps, uint32_t trainEnd, uint32_t srIndex)
{
    if (srIndex < 1)
    {
        return false;
    }

    const auto& sr = backlog[srIndex];
    NetworkQueue queue;
    queue.onPacketSent(sr.transmissionTime, sr.size);

    for (uint32_t i = srIndex - 1;; --i)
    {
        const auto& item = backlog[i];
        queue.onPacketSent(item.transmissionTime, item.size);
        if (queue.size() <= item.size)
        {
            return false;
        }

        if (i == trainEnd)
        {
            return true;
        }
    }

    return false;
}

} // namespace

RateController::BacklogAnalysis RateController::bestReport(const RateController::BacklogAnalysis& probe1,
    const RateController::BacklogAnalysis& probe2,
    const uint32_t modelBandwidth) const
{
    if (isGood(probe2, modelBandwidth))
    {
        if (isGood(probe1, modelBandwidth) &&
            probe2.delaySinceSR * probe1.receivedAfterSR > probe1.delaySinceSR * probe2.receivedAfterSR)
        {
            return probe1;
        }
        return probe2;
    }

    return probe1;
}

bool RateController::isGood(const RateController::BacklogAnalysis& probe, const uint32_t modelBandwidthKbps) const
{
    const bool validMeasurement = (probe.delaySinceSR > ntp32Second / 5 && probe.getBitrateKbps() > modelBandwidthKbps);
    const bool validProbe = probe.probing && static_cast<double>(probe.receivedAfterSR) / probe.sentAfterSR > 0.9 &&
        probe.delaySinceSR > ntp32Second / 10 && probe.senderReportItem;

    return probe.senderReportItem != nullptr && (validMeasurement || validProbe);
}

RateController::BacklogAnalysis RateController::analyzeProbe(const uint32_t probeEndIndex,
    const double modelBandwidthKbps) const
{
    BacklogAnalysis report;

    const PacketMetaData& probeEndItem = _backlog[probeEndIndex];
    report.receivedAfterSR = probeEndItem.size;
    report.delaySinceSR = probeEndItem.delaySinceSR;
    report.packetsSent = 1;
    report.packetsReceived = 1;

    auto lossSinceSR = probeEndItem.lossCount;
    const auto transmissionEnd = probeEndItem.transmissionTime;
    auto largestPacketReceived = probeEndItem.size;

    for (uint32_t i = probeEndIndex + 1; i < _backlog.size(); ++i)
    {
        auto& item = _backlog[i];
        if (item.type == PacketMetaData::SR && item.reportNtp == probeEndItem.reportNtp)
        {
            report.transmitPeriod = transmissionEnd - item.transmissionTime;

            if (report.receivedAfterSR > 0)
            {
                report.probing = isProbe(_backlog, modelBandwidthKbps, probeEndIndex, i);
                report.receivedAfterSR =
                    report.receivedAfterSR - std::min(report.receivedAfterSR, lossSinceSR * largestPacketReceived);
            }
            report.senderReportItem = &item;
            report.lossCount = lossSinceSR;
            report.lossRatio = static_cast<double>(lossSinceSR) / std::max(1u, report.packetsSent);
            return report;
        }
        else
        {
            if (item.type == PacketMetaData::RTCP_PADDING)
            {
                report.rtpProbe = false;
            }
            if (item.received || item.type == PacketMetaData::RTCP_PADDING)
            {
                report.receivedAfterSR += item.size;
                ++report.packetsReceived;
            }

            lossSinceSR += item.lossCount;
            largestPacketReceived = std::max(item.size, largestPacketReceived);
            if (item.type == PacketMetaData::RTP || item.type == PacketMetaData::RTCP_PADDING)
            {
                // could include sctp and other SR
                ++report.packetsSent;
                report.sentAfterSR += item.size;
            }
        }
    }

    return report;
}

/**
 *  Walk the backlog and find complete probe sequences where no packets are lost and receive time is at least 50ms.
 * Multiple receiver reports may be needed to complete such probes.
 * The RR may indicate that we have loss due to sequence number gaps, but in reality we have received all packets in the
 * sequence because all of those that were sent were also received.
 */
RateController::BacklogAnalysis RateController::analyzeBacklog(uint32_t reportNtpLimit, double modelBandwidthKbps) const
{
    BacklogAnalysis report;

    bool hasReceived = false;
    uint32_t networkQueue = 0;
    for (uint32_t i = 0; i < _backlog.size(); ++i)
    {
        auto& item = _backlog[i];

        if (item.type == PacketMetaData::RTP && item.reportNtp != 0 && item.received &&
            item.delaySinceSR > ntp32Second / 20)
        {
            auto reportCandidate = analyzeProbe(i, modelBandwidthKbps);
            if (reportCandidate.lossCount > 0 && report.empty())
            {
                report = reportCandidate;
                report.networkQueue = networkQueue;
                return report;
            }

            if (isGood(reportCandidate, modelBandwidthKbps))
            {
                RCTL_LOG("Candidate found delay %.3f, %uB, %ukbps, ntp %x txrx %u-%u",
                    _logId.c_str(),
                    reportCandidate.delaySinceSR * 1000.0 / 0x10000u,
                    reportCandidate.receivedAfterSR,
                    reportCandidate.getBitrateKbps(),
                    (reportCandidate.senderReportItem ? reportCandidate.senderReportItem->reportNtp : 0),
                    reportCandidate.packetsSent,
                    reportCandidate.packetsReceived);
            }
            report = bestReport(reportCandidate, report, modelBandwidthKbps);
        }

        if (item.received && !hasReceived)
        {
            hasReceived = true;
        }
        else if (!hasReceived)
        {
            networkQueue += item.size;
        }

        if (item.type == PacketMetaData::SR && static_cast<int32_t>(item.reportNtp - reportNtpLimit) < 0)
        {
            break;
        }
    }

    report.networkQueue = networkQueue;
    return report;
}

void RateController::dumpBacklog(const uint32_t seqno, const uint32_t ssrc)
{
    uint32_t queueSize = 0;

    for (auto& p : _backlog)
    {
        if (p.sequenceNumber == seqno && ssrc == p.ssrc)
        {
            queueSize = p.size;
        }
        else if (queueSize > 0)
        {
            queueSize += p.size;
        }

        if (p.type == PacketMetaData::SR)
        {
            logger::debug("SR ssrc %u:%u, %x, sz %u, origQ %u, %" PRIu64 " Q %u",
                _logId.c_str(),
                p.ssrc,
                ssrc,
                p.reportNtp,
                p.size,
                p.queueSize,
                p.transmissionTime,
                queueSize);
        }
        else
        {
            logger::debug("pkt type %u ssrc %u, seq %u, sz %u, loss %u, %" PRIu64 " Q %u, rx %u, ntp %x",
                _logId.c_str(),
                p.type,
                p.ssrc,
                p.sequenceNumber,
                p.size,
                p.lossCount,
                p.transmissionTime,
                queueSize,
                p.received,
                p.reportNtp);
        }

        if (static_cast<int32_t>(seqno - p.sequenceNumber) > 1 && ssrc == p.ssrc && p.reportNtp != 0 &&
            p.type == PacketMetaData::RTP)
        {
            break;
        }
    }
}

void RateController::onReportReceived(uint64_t timestamp,
    uint32_t count,
    const rtp::ReportBlock blocks[],
    uint32_t rttNtp)
{
    if (!_config.enabled)
    {
        return;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& rb = blocks[i];
        RCTL_LOG("RB received %u, seq %u, ntp %x, %ums, loss %u, Q %u, tQ %u",
            _logId.c_str(),
            rb.ssrc.get(),
            rb.extendedSeqNoReceived.get(),
            rb.lastSR.get(),
            rb.delaySinceLastSR * 1000 / ntp32Second,
            rb.loss.getCumulativeLoss(),
            _model.queue.size(),
            _model.targetQueue);

        //  dumpBacklog(rb.extendedSeqNoReceived, rb.ssrc);
    }

    _minRttNtp = std::min(_minRttNtp, rttNtp);

    uint32_t limitNtp = blocks[0].lastSR;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& block = blocks[i];
        if (block.lastSR != 0 && static_cast<int32_t>(limitNtp - block.lastSR) > 0)
        {
            limitNtp = block.lastSR;
        }
    }
    if (limitNtp == 0)
    {
        return;
    }

    auto backlogReport = analyzeBacklog(limitNtp - ntp32Second * 10, _model.queue.getBandwidth());
    if (backlogReport.networkQueue == ~0u || backlogReport.receivedAfterSR == 0 || !backlogReport.senderReportItem)
    {
        return;
    }

    bool sameProbe = false;
    if (backlogReport.senderReportItem->reportNtp == _lastProbe.ntp &&
        backlogReport.delaySinceSR == _lastProbe.delaySinceSR &&
        backlogReport.packetsReceived == _lastProbe.packetsReceived)
    {
        sameProbe = true;
    }
    else
    {
        _lastProbe.ntp = backlogReport.senderReportItem->reportNtp;
        _lastProbe.delaySinceSR = backlogReport.delaySinceSR;
        _lastProbe.packetsReceived = backlogReport.packetsReceived;
        RCTL_LOG("delaySinceSR %.1fms, ntp %x loss %u, lossRatio %.3f, %uB, rxRate %ukbps, txRate %ukbps found SR %s, "
                 "probe %u, rtpProbe %u networkQ %u, txrx %u %u",
            _logId.c_str(),
            static_cast<double>(backlogReport.delaySinceSR * 1000 / ntp32Second),
            backlogReport.senderReportItem->reportNtp,
            backlogReport.lossCount,
            backlogReport.lossRatio,
            backlogReport.receivedAfterSR,
            backlogReport.getBitrateKbps(),
            backlogReport.getSendRateKbps(),
            backlogReport.senderReportItem ? "t" : "f",
            backlogReport.probing,
            backlogReport.rtpProbe,
            backlogReport.networkQueue,
            backlogReport.packetsSent,
            backlogReport.packetsReceived);
    }

    const uint32_t receiveRateKbps = backlogReport.rtpProbe
        ? std::min(_config.bandwidthCeilingKbps, backlogReport.getBitrateKbps())
        : std::min(_config.rtcpProbeCeiling, backlogReport.getBitrateKbps());

    const auto currentEstimate = _model.queue.getBandwidth();

    if (!sameProbe && isGood(backlogReport, currentEstimate) && backlogReport.lossCount < 3 &&
        backlogReport.lossRatio <= 0.02)
    {
        if (receiveRateKbps > _model.queue.getBandwidth())
        {
            _model.queue.setBandwidth(std::min(currentEstimate + 1000, backlogReport.getBitrateKbps()));
        }
        else if (backlogReport.networkQueue + _config.mtu < _model.queue.size())
        {
            // More rare case. We did not get useful info about receive rate but we see that the network
            // queue is shorter than anticipated
            _model.queue.setBandwidth(std::min(currentEstimate, _config.bandwidthCeilingKbps));
            RCTL_LOG("increased rc estimate due to short network queue 200kbps, to %.1fkbps",
                _logId.c_str(),
                currentEstimate * 0.02);

            _model.queue.setBandwidth(currentEstimate * 1.02);
        }
        else if (backlogReport.packetsSent > 4)
        {
            _model.queue.setBandwidth(currentEstimate - 0.1 * (currentEstimate - receiveRateKbps));
        }
    }
    else if (!sameProbe && backlogReport.lossCount > 2 && backlogReport.lossRatio > 0.02 &&
        backlogReport.networkQueue > _model.targetQueue / 2 &&
        (_lastLossBackoff == 0 || utils::Time::diffGE(_lastLossBackoff, timestamp, utils::Time::sec * 5)))
    {
        _model.queue.setBandwidth(std::max(_config.bandwidthFloorKbps, static_cast<uint32_t>(currentEstimate * 0.95)));
        _lastLossBackoff = timestamp;
        RCTL_LOG("decrease due to loss %.3f %ukbps, targetQ %u",
            _logId.c_str(),
            backlogReport.lossRatio,
            _model.queue.getBandwidth(),
            _model.targetQueue);
    }

    _model.targetQueue = calculateTargetQueue(_model.queue.getBandwidth(), _minRttNtp, _config);
    _model.queue.setBandwidth(
        math::clamp(_model.queue.getBandwidth(), _config.bandwidthFloorKbps, _config.bandwidthCeilingKbps));
    if (_model.queue.size() > backlogReport.networkQueue)
    {
        _model.queue.setSize(backlogReport.networkQueue);
    }

    RCTL_LOG("model %ukbps mQ %uB tQ %uB, nwQueue %u, rxRate %ukbps rtt %.1fms loss "
             "%u, probing %s",
        _logId.c_str(),
        _model.queue.getBandwidth(),
        _model.queue.size(),
        _model.targetQueue,
        backlogReport.networkQueue,
        receiveRateKbps,
        double(rttNtp) * 1000 / ntp32Second,
        backlogReport.lossCount,
        backlogReport.probing ? "t" : "f");

    _model.queue.setSize(std::min(_model.queue.size(), backlogReport.networkQueue));

    if (_probe.duration != 0 &&
        (backlogReport.lossCount > 0 || utils::Time::diffGE(_probe.start, timestamp, _probe.duration)))
    {
        // there may be later RB after SR and need good measurements of bw
        RCTL_LOG("stopping probe", _logId.c_str());
        _probe.duration = 0;
    }
}

void RateController::onRtcpPaddingSent(uint64_t timestamp, uint32_t ssrc, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    _model.queue.onPacketSent(timestamp, size + _config.ipOverhead);

    if (!_backlog.empty())
    {
        auto& item = _backlog.front();
        if (item.type == PacketMetaData::RTCP_PADDING && item.transmissionTime == timestamp)
        {
            item.size += size;
            return;
        }
    }

    _backlog.emplace_front(timestamp, ssrc, 0, size + _config.ipOverhead, PacketMetaData::RTCP_PADDING);
}

void RateController::onSctpSent(uint64_t timestamp, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    _model.queue.onPacketSent(timestamp, size + _config.ipOverhead);
    _backlog.emplace_front(timestamp, 0, 0, size + _config.ipOverhead, PacketMetaData::SCTP);
}

uint32_t RateController::getPadding(const uint64_t timestamp, const uint16_t size, uint16_t& paddingSize) const
{
    if (!_config.enabled)
    {
        return 0;
    }

    const auto budget = getPacingBudget(timestamp);

    if (budget == 0 || _probe.start == 0 || utils::Time::diffGE(_probe.start, timestamp, _probe.duration))
    {
        if (_canRtxPad &&
            (_rtxSendTime == 0 || utils::Time::diffGE(_rtxSendTime, timestamp, _config.minPadPinInterval)))
        {
            paddingSize = rtp::MIN_RTP_HEADER_SIZE + 4; // tiny packet to keep flow on padding ssrc
            return 1;
        }
        return 0;
    }

    if (budget >= size)
    {
        const auto batchSize = budget - size;
        const auto count = 1 + batchSize / _config.maxPaddingSize;
        paddingSize = batchSize / count;
        if (paddingSize < _config.minPadSize)
        {
            return 0;
        }

        return count;
    }
    return 0;
}

double RateController::calculateSendRate(const uint64_t timestamp) const
{
    double byteCount = 0;

    for (const auto& item : _backlog)
    {
        byteCount += item.size;
        auto period = timestamp - item.transmissionTime;
        if (period > utils::Time::ms * 600)
        {

            return byteCount * 8 * utils::Time::ms / period;
        }
    }

    return 0;
}

size_t RateController::getPacingBudget(uint64_t timestamp) const
{
    const auto currentQueue = _model.queue.predictQueueAt(timestamp);

    auto currentTargetQ = _model.targetQueue;
    if (_probe.isProbing(timestamp))
    {
        currentTargetQ += _config.mtu;
        currentTargetQ +=
            static_cast<uint32_t>(_model.targetQueue * 2 * (timestamp - _probe.start) / _config.probeDuration);
    }

    return currentTargetQ - std::min(currentTargetQ, currentQueue);
}

double RateController::getTargetRate() const
{
    if (!_config.enabled)
    {
        return 0;
    }

    return _model.queue.getBandwidth() * (1.0 - _drainMargin);
}

} // namespace bwe
