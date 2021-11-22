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

#define RCTL_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)

namespace bwe
{
const uint32_t ntp32Second = 0x10000u;

RateController::RateController(size_t instanceId, const RateControllerConfig& config)
    : _logId("RateCtrl", instanceId),
      _minRttNtp(~0u),
      _config(config)
{
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

    _model.onPacketSent(timestamp, size + _config.ipOverhead);
    _backlog.emplace_front(timestamp, ssrc, sequenceNumber, size + _config.ipOverhead, PacketMetaData::RTP);
}

void RateController::onSenderReportSent(uint64_t timestamp, uint32_t ssrc, uint32_t reportNtp, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    _model.onPacketSent(timestamp, size + _config.ipOverhead);

    PacketMetaData metaPacket(timestamp, ssrc, 0, size, PacketMetaData::SR);
    metaPacket.reportNtp = reportNtp;
    metaPacket.queueSize = _model.queue;
    _backlog.push_front(metaPacket);
    logger::debug("SR sent ssrc %u, size %u, ntp %x", _logId.c_str(), ssrc, size, reportNtp);

    const auto budget = getPacingBudget(timestamp);

    const bool canSendProbe =
        budget > 0 && (_probe.start == 0 || utils::Time::diffGE(_probe.start, timestamp, _probe.interval));

    if ((_probe.count < 5 && _probe.duration == 0) || canSendProbe)
    {
        _probe.start = timestamp;
        _probe.duration = 700 * utils::Time::ms; // could take previous SR to RR delay into account
        ++_probe.count;
        RCTL_LOG("starting probe %" PRIu64 "ms target Q %uB",
            _logId.c_str(),
            _probe.duration / utils::Time::ms,
            _model.targetQueue);
    }
}

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

    // when walking we should calc expected loss due to sequence number gaps and if reported loss does not exceed that,
    // we do have a loss free sequence.
    uint32_t receivedPackets = 0;
    for (auto& item : _backlog)
    {
        if (item.ssrc == ssrc && item.type == PacketMetaData::RTP)
        {
            if (item.sequenceNumber == receivedSequenceNumber)
            {
                item.received = true;
                item.lossCount = lossCount;
                item.reportNtp = reportNtp;
                item.delaySinceSR = delaySinceSR;
                ++receivedPackets;
                if (lossCount > 0)
                {
                    logger::debug("marking %u, %u with loss %u", _logId.c_str(), ssrc, item.sequenceNumber, lossCount);
                }
            }
            else if (item.received)
            {
                break; // part of previous report on this ssrc
                // at this point we could walk forward and mark packets to sacrifice as loss according to lossCount.
                // That would give us ability to assess bandwidth also when some loss is present
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
        dumpBacklog(receivedSequenceNumber, ssrc);
    }
}

namespace
{
template <typename T>
bool isProbe(const T& backlog, double bandwidthKbps, uint32_t trainEnd, uint32_t srIndex)
{
    if (srIndex < 1)
    {
        return false;
    }

    const auto& sr = backlog[srIndex];
    uint32_t queueSize = sr.size + sr.queueSize;
    uint64_t lastTransmission = sr.transmissionTime;

    for (uint32_t i = srIndex - 1;; --i)
    {
        const auto& item = backlog[i];
        queueSize -= std::min(queueSize,
            static_cast<uint32_t>(
                utils::Time::diff(lastTransmission, item.transmissionTime) * bandwidthKbps / (8 * utils::Time::ms)));
        if (queueSize < std::min(sr.size, 100u))
        {
            return false;
        }

        queueSize += item.size;
        lastTransmission = item.transmissionTime;

        if (i == 0)
        {
            return queueSize > 100;
        }
    }

    return false;
}
} // namespace

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
                ++report.packetsSent;
            }
        }
    }

    return report;
}

/**
 *  Walk the backlog and find complete probe sequences where no packets are lost adn receive time is at least 50ms.
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
            if (reportCandidate.probing && reportCandidate.lossRatio <= 0.02)
            {
                if (report.getBitrateKbps() < reportCandidate.getBitrateKbps())
                {
                    report = reportCandidate;
                }
            }
            if (reportCandidate.lossCount > 0)
            {
                if (report.senderReportItem)
                {
                    return report;
                }
                logger::debug("halt at probe ending at %u, seq %u, ntp %x loss %u",
                    _logId.c_str(),
                    item.ssrc,
                    item.sequenceNumber,
                    item.reportNtp,
                    item.lossCount);
                report = reportCandidate;
                report.networkQueue = networkQueue;
                return report;
            }
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
        auto& rb = blocks[i];
        logger::debug("RB received %u, seq %u, ntp %x, %ums, loss %u",
            _logId.c_str(),
            rb.ssrc.get(),
            rb.extendedSeqNoReceived.get(),
            rb.lastSR.get(),
            rb.delaySinceLastSR * 1000 / ntp32Second,
            rb.loss.getCumulativeLoss());
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

    auto backlogReport = analyzeBacklog(limitNtp - ntp32Second * 9, _model.bandwidthKbps);
    if (backlogReport.networkQueue == ~0u || backlogReport.receivedAfterSR == 0 || !backlogReport.senderReportItem)
    {
        return;
    }

    double receiveRateKbps = backlogReport.rtpProbe
        ? std::min(_config.bandwidthCeilingKbps, backlogReport.getBitrateKbps())
        : std::min(_config.rtcpProbeCeiling, backlogReport.getBitrateKbps());

    RCTL_LOG("delaySinceSR %.1fms, ntp %x loss %u, lossRatio %.3f, %uB, rxRate %.2fkbps, txRate %.1fkbps found SR %s, "
             "probe %u, rtpProbe %u networkQ %u",
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
        backlogReport.networkQueue);

    if (backlogReport.lossCount < 3 && backlogReport.lossRatio <= 0.02)
    {
        const auto bandwidthChangeKbps = receiveRateKbps - std::min(receiveRateKbps, _model.bandwidthKbps);
        const auto timeSinceSR = timestamp - backlogReport.senderReportItem->transmissionTime;
        if (backlogReport.delaySinceSR > ntp32Second / 20 && receiveRateKbps > _model.bandwidthKbps)
        {
            const auto extraDrain =
                std::min(static_cast<double>(_model.queue), timeSinceSR * bandwidthChangeKbps / (8 * utils::Time::ms));

            if (receiveRateKbps > _model.bandwidthKbps)
            {
                _model.bandwidthKbps = backlogReport.getBitrateKbps();

                if (backlogReport.probing)
                {
                    _model.queue -= extraDrain;
                }
                logger::debug("bw increase drained %.1f", _logId.c_str(), extraDrain);
            }
            else
            {
                _model.bandwidthKbps -= 0.1 * (_model.bandwidthKbps - receiveRateKbps);
                logger::debug("probe decrease", _logId.c_str());
            }
        }
        else if (backlogReport.networkQueue + _config.mtu < _model.queue)
        {
            // More rare case. We did not get useful info about receive rate but we see that the network
            // queue is shorter than anticipated
            _model.bandwidthKbps += 100.0;
            _model.bandwidthKbps = std::min(_model.bandwidthKbps, _config.bandwidthCeilingKbps);
            logger::debug("increased rc estimate due to short network queue 200kbps, to %.1fkbps",
                _logId.c_str(),
                _model.bandwidthKbps);
        }

        if (_minRttNtp != ~0u)
        {
            // aim at 100ms queue build up. Some network buffers are 75K so it is wise to not exceed
            _model.targetQueue =
                std::max(_model.bandwidthKbps, receiveRateKbps) * (80 + _minRttNtp * 500 / ntp32Second) / 8;
        }
    }
    else if (backlogReport.lossCount > 2 && backlogReport.lossRatio > 0.02 &&
        backlogReport.networkQueue > _model.targetQueue / 2 &&
        (_lastLossBackoff == 0 || utils::Time::diffGE(_lastLossBackoff, timestamp, utils::Time::sec * 5)))
    {
        _model.bandwidthKbps = std::max(_config.bandwidthFloorKbps, _model.bandwidthKbps * 0.85);

        _model.targetQueue = _model.bandwidthKbps * (80 + _minRttNtp * 500 / ntp32Second) / 8;
        _lastLossBackoff = timestamp;
        logger::debug("decrease due to loss %.3f %.1fkbps, targetQ %u",
            _logId.c_str(),
            backlogReport.lossRatio,
            _model.bandwidthKbps,
            _model.targetQueue);
    }

    _model.bandwidthKbps = math::clamp(_model.bandwidthKbps, _config.bandwidthFloorKbps, _config.bandwidthCeilingKbps);
    _model.targetQueue = math::clamp(_model.targetQueue, 4 * _config.mtu, _config.maxTargetQueue);
    if (_model.queue > backlogReport.networkQueue)
    {
        logger::debug("nwqueue shorter ", _logId.c_str());
        _model.queue = backlogReport.networkQueue;
    }

    RCTL_LOG("model %.1fkbps mQ %uB tQ %uB, nwQueue %u, rxRate %.fkbps rtt %.1fms loss "
             "%u, probing %s",
        _logId.c_str(),
        _model.bandwidthKbps,
        _model.queue,
        _model.targetQueue,
        backlogReport.networkQueue,
        receiveRateKbps,
        double(rttNtp) * 1000 / ntp32Second,
        backlogReport.lossCount,
        backlogReport.probing ? "t" : "f");
    _model.queue = std::min(_model.queue, backlogReport.networkQueue);

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

    _model.onPacketSent(timestamp, size + _config.ipOverhead);

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

    _model.onPacketSent(timestamp, size + _config.ipOverhead);
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
        const auto batchSize = std::min(budget - size, static_cast<size_t>(_config.mtu * 4));
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
    const auto currentQueue = _model.queue -
        std::min(_model.queue,
            static_cast<uint32_t>(
                _model.bandwidthKbps * (timestamp - _model.lastTransmission) / (8 * utils::Time::ms)));

    return _model.targetQueue - std::min(_model.targetQueue, currentQueue);
}

} // namespace bwe
