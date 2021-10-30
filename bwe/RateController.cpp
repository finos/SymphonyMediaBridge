#include "bwe/RateController.h"
#include "logger/Logger.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/CheckedCast.h"
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
    _backlog.emplace_back(timestamp, ssrc, sequenceNumber, size + _config.ipOverhead, PacketMetaData::RTP);
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
    _backlog.push_back(metaPacket);
    RCTL_LOG("SR sent ssrc %u, size %u, ntp %x", _logId.c_str(), ssrc, size, reportNtp);

    if (_model.networkQueue < _model.targetQueue &&
        (_probe.start == 0 || utils::Time::diffGE(_probe.start, timestamp, _probe.interval)))
    {
        RCTL_LOG("starting pad", _logId.c_str());
        _probe.start = timestamp;
        _probe.duration = 700 * utils::Time::ms; // could take previous SR to RR delay into account
        _probe.initialQueue = _model.networkQueue;
    }
}

void RateController::onReportBlockReceived(uint32_t ssrc,
    uint32_t receivedSequenceNumber,
    uint32_t cumulativeLossCount,
    uint32_t reportNtp)
{
    if (!_config.enabled)
    {
        return;
    }

    ReceiveBlockSample* sample = nullptr;
    for (size_t i = _receiveBlocks.headIndex(); i < _receiveBlocks.tailIndex(); ++i)
    {
        auto& sampleBlock = _receiveBlocks[i];
        if (sampleBlock.empty())
        {
            break;
        }
        if (sampleBlock.ssrc == ssrc)
        {
            sample = &sampleBlock;
        }
    }
    if (!sample)
    {
        _receiveBlocks.emplace_back(ssrc);
        sample = &_receiveBlocks.tail();
    }

    const uint32_t lossCount = cumulativeLossCount - std::min(cumulativeLossCount, sample->lossCount);
    sample->lossCount = cumulativeLossCount;

    for (size_t i = _backlog.tailIndex(); i != _backlog.headIndex(); --i)
    {
        auto& item = _backlog[i];

        if (item.ssrc == ssrc && (item.type == PacketMetaData::RTP || item.type == PacketMetaData::RTP_PADDING) &&
            item.sequenceNumber == receivedSequenceNumber)
        {
            item.received = true;
            item.lossCount = lossCount;
            return;
        }
    }
}

namespace
{
template <typename T>
bool isProbe(const T& backlog, double bandwidthKbps, size_t srIndex, size_t tailIndex)
{
    const auto& sr = backlog[srIndex];
    uint32_t queueSize = sr.queueSize;
    uint64_t lastTransmission = sr.transmissionTime;

    for (size_t i = srIndex + 1; i <= tailIndex; ++i)
    {
        const auto& item = backlog[i];
        queueSize -= std::min(queueSize,
            static_cast<uint32_t>(
                utils::Time::diff(lastTransmission, item.transmissionTime) * bandwidthKbps / (8 * utils::Time::ms)));
        if (queueSize < 100)
        {
            return false;
        }

        queueSize += item.size;
        lastTransmission = item.transmissionTime;
    }

    return queueSize > 100;
}
} // namespace

// Walk through the backlog from tail up to SR. Extract how many bytes has not been received and how many bytes has been
// received after the SR. This will hint us on receive rate and how much is still queued in the network.
RateController::PacketMetaData* RateController::analyzeBacklog(uint32_t ssrc,
    uint32_t reportNtp,
    uint32_t receivedSequenceNumber,
    double modelBandwidthKbps,
    uint64_t& transmissionPeriod,
    uint32_t& receivedAfterSR,
    uint32_t& lossSinceSR,
    uint32_t& networkQueue,
    bool& probing)
{
    transmissionPeriod = 0;
    receivedAfterSR = 0;
    networkQueue = 0;
    lossSinceSR = 0;
    probing = false;

    size_t tailIndex = 0;
    bool hasReceived = false;
    uint64_t transmissionEnd = 0;
    for (size_t i = 0; i < _backlog.size(); ++i)
    {
        auto& item = _backlog[_backlog.tailIndex() - i];

        if (item.type == PacketMetaData::SR && item.reportNtp == reportNtp)
        {
            transmissionPeriod = transmissionEnd - item.transmissionTime;

            if (receivedAfterSR > 0)
            {
                probing = isProbe(_backlog, modelBandwidthKbps, _backlog.tailIndex() - i, tailIndex);
            }
            return &item;
        }
        else if ((item.type == PacketMetaData::RTP || item.type == PacketMetaData::RTP_PADDING) && item.ssrc == ssrc &&
            item.sequenceNumber == receivedSequenceNumber)
        {
            if (receivedAfterSR == 0)
            {
                tailIndex = _backlog.tailIndex() - i;
            }
            item.received = true;
            lossSinceSR += item.lossCount;
            transmissionEnd = item.transmissionTime;
            receivedAfterSR += item.size;
        }
        else if (receivedAfterSR > 0)
        {
            receivedAfterSR += item.size;
        }
        else if (item.received)
        {
            hasReceived = true;
        }
        else if (!hasReceived)
        {
            networkQueue += item.size;
        }
    }

    return nullptr;
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

    _minRttNtp = std::min(_minRttNtp, rttNtp);

    uint32_t networkQueue = ~0u;
    double maxRateKbps = 0;
    uint32_t lossCount = 0;
    bool isProbing = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint64_t transmissionPeriod = 0;
        uint32_t receivedAfterSR = 0;
        uint32_t lossSinceSR = 0;
        uint32_t estimatedQueue = 0;
        bool isaProbe = false;

        const auto& block = blocks[i];
        auto* item = analyzeBacklog(block.ssrc,
            block.lastSR,
            block.extendedSeqNoReceived,
            _model.bandwidthKbps,
            transmissionPeriod,
            receivedAfterSR,
            lossSinceSR,
            estimatedQueue,
            isaProbe);

        RCTL_LOG("%u delaySinceSR %.1fms, seqno %u, loss %u, lastSR %x %uB, rxRate %.2fkbps, found SR %s, probe %u",
            _logId.c_str(),
            block.ssrc.get(),
            static_cast<double>(block.delaySinceLastSR.get() * 1000 / ntp32Second),
            block.extendedSeqNoReceived.get(),
            lossSinceSR,
            block.lastSR.get(),
            receivedAfterSR,
            block.delaySinceLastSR.get() > 0
                ? static_cast<double>(receivedAfterSR) * ntp32Second / (125 * block.delaySinceLastSR)
                : 0.0,
            item ? "t" : "f",
            isaProbe);

        networkQueue = std::min(estimatedQueue, networkQueue);
        if (item && receivedAfterSR > 0)
        {
            if (block.delaySinceLastSR > ntp32Second / 20 && lossSinceSR == 0 && isaProbe)
            {
                maxRateKbps = std::max(maxRateKbps,
                    static_cast<double>(receivedAfterSR) * ntp32Second / (125 * block.delaySinceLastSR));
                isProbing = true;
            }
            lossCount = std::max(lossSinceSR, lossCount);
        }
    }
    if (networkQueue == ~0u)
    {
        return;
    }

    if (lossCount == 0)
    {
        _model.networkQueue = networkQueue;

        if (isProbing && _model.queue > _model.targetQueue / 5 && maxRateKbps != 0)
        {
            _model.bandwidthKbps +=
                (maxRateKbps > _model.bandwidthKbps ? 1.0 : 0.1) * (maxRateKbps - _model.bandwidthKbps);
        }
        else if (isProbing && networkQueue + _config.mtu < _model.queue && timestamp - _probe.start > 0)
        {
            const auto extraBw = (_model.queue - std::min(_model.queue, networkQueue + _probe.initialQueue)) * 8 *
                utils::Time::ms / (timestamp - _probe.start);
            _model.bandwidthKbps += extraBw;
        }
        else if (isProbing)
        {
            _model.bandwidthKbps +=
                (maxRateKbps > _model.bandwidthKbps ? 0.5 : 0.05) * (maxRateKbps - _model.bandwidthKbps);
        }

        if (_minRttNtp != ~0u && _minRttNtp != 0)
        {
            // aim at 100ms queue build up. Some network buffers are 75K so it is wise to not exceed
            _model.targetQueue =
                std::max(_model.bandwidthKbps, maxRateKbps) * (80 + _minRttNtp * 1000 / ntp32Second) / 8;
            _model.targetQueue = std::max(_model.targetQueue, 4 * _config.mtu);
            _model.targetQueue = std::min(_model.targetQueue, 60000u);
        }
        else
        {
            _model.targetQueue += _config.mtu;
        }
    }
    else if (lossCount > 2 && (rttNtp - _minRttNtp) > (100 * ntp32Second) / 1000)
    {
        _model.targetQueue = _model.targetQueue / 2;
        _model.bandwidthKbps = std::max(static_cast<double>(_config.bandwidthFloorKbps), _model.bandwidthKbps * 0.85);
    }

    _model.bandwidthKbps = std::min(_model.bandwidthKbps, static_cast<double>(_config.bandwidthCeilingKbps));
    _model.bandwidthKbps = std::max(_model.bandwidthKbps, static_cast<double>(_config.bandwidthFloorKbps));
    _model.targetQueue = std::max(4 * _config.mtu, _model.targetQueue);

    auto sendRate = calculateSendRate(timestamp);
    RCTL_LOG("model %.1fkbps mQ %uB tQ %uB, netQueue %u, rxRate %.fkbps txRate %.fkbps rtt %.1fms loss %u, probing %s",
        _logId.c_str(),
        _model.bandwidthKbps,
        _model.queue,
        _model.targetQueue,
        networkQueue,
        maxRateKbps,
        sendRate,
        double(rttNtp) * 1000 / ntp32Second,
        lossCount,
        isProbing ? "t" : "f");
    _model.queue = std::min(_model.queue, networkQueue);

    if (isProbing && _probe.duration != 0 &&
        (lossCount > 0 || networkQueue > _model.targetQueue ||
            utils::Time::diffGE(_probe.start, timestamp, utils::Time::ms * 400)))
    {
        // there may be later RB after SR and need good measurements of bw
        RCTL_LOG("turning probe off", _logId.c_str());
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
        auto& item = _backlog.tail();
        if (item.type == PacketMetaData::RTCP_PADDING && item.transmissionTime == timestamp)
        {
            item.size += size;
            return;
        }
    }

    _backlog.emplace_back(timestamp, ssrc, 0, size + _config.ipOverhead, PacketMetaData::RTCP_PADDING);
}

void RateController::onRtpPaddingSent(uint64_t timestamp, uint32_t ssrc, uint32_t sequenceNumber, uint16_t size)
{
    if (size == 0 || !_config.enabled)
    {
        return;
    }

    _model.onPacketSent(timestamp, size + _config.ipOverhead);
    _backlog.emplace_back(timestamp, ssrc, sequenceNumber, size + _config.ipOverhead, PacketMetaData::RTP_PADDING);
}

uint32_t RateController::getPadding(const uint64_t timestamp,
    const uint32_t ssrc,
    const uint16_t size,
    uint16_t& paddingSize)
{
    if (!_config.enabled)
    {
        return 0;
    }

    if (_model.networkQueue > _model.targetQueue || _probe.start == 0 ||
        utils::Time::diffGE(_probe.start, timestamp, _probe.duration))
    {
        if (_probe.lastPaddingSendTime == 0 ||
            utils::Time::diffGE(_probe.lastPaddingSendTime, timestamp, _config.minPadPinInterval))
        {
            _probe.lastPaddingSendTime = timestamp;
            paddingSize = rtp::MIN_RTP_HEADER_SIZE + 4; // tiny packet to keep flow on padding ssrc
            return 1;
        }
        return 0;
    }

    if (_model.queue + size < _model.targetQueue)
    {
        auto batchSize = std::min(_model.targetQueue - _model.queue, _config.mtu * 5) - size;
        auto count = 1 + batchSize / _config.maxPaddingSize;
        paddingSize = batchSize / count;
        if (paddingSize < _config.minPadSize)
        {
            return 0;
        }
        _probe.lastPaddingSendTime = timestamp;
        return count;
    }
    return 0;
}

double RateController::calculateSendRate(const uint64_t timestamp) const
{
    double byteCount = 0;

    for (size_t i = 0; i < _backlog.size(); ++i)
    {
        const auto& item = _backlog[_backlog.tailIndex() - i];
        byteCount += item.size;
        auto period = timestamp - item.transmissionTime;
        if (period > utils::Time::ms * 600)
        {
            return byteCount * 8 * utils::Time::ms / period;
        }
    }

    return 0;
}
} // namespace bwe
