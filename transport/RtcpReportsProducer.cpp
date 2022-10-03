#include "RtcpReportProducer.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include <algorithm>

using namespace transport;

namespace
{

bool ensurePacket(memory::UniquePacket& packet, memory::PacketPoolAllocator& allocator)
{
    if (!packet)
    {
        packet = memory::makeUniquePacket(allocator);
    }

    return !!packet;
}

utils::Span<uint32_t> removeAndGetLastFromReport(utils::Span<uint32_t>& ssrcReport, size_t n)
{
    const auto removedSpan = ssrcReport.subSpan(ssrcReport.size() - n);
    ssrcReport = ssrcReport.subSpan(0, ssrcReport.size() - n);
    return removedSpan;
}

} // namespace

RtcpReportProducer::RtcpReportProducer(const logger::LoggableId& loggableId,
    const config::Config& config,
    const concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& outboundSsrcCounters,
    const concurrency::MpmcHashmap32<uint32_t, RtpReceiveState>& inboundSsrcCounters,
    memory::PacketPoolAllocator& rtcpPacketAllocator,
    RtcpSender& rtcpSender)
    : _loggableId(loggableId),
      _config(config),
      _outboundSsrcCounters(outboundSsrcCounters),
      _inboundSsrcCounters(inboundSsrcCounters),
      _rtcpPacketAllocator(rtcpPacketAllocator),
      _rtcpSender(rtcpSender)
{
}

// Send sender reports and receiver reports as needed for the inbound and outbound ssrcs.
// each sender report send time is offset 1/65536 sec to make them unique when we look them up
// after receive blocks arrive as they reference the SR by ntp timestamp
bool RtcpReportProducer::sendReports(uint64_t timestamp, const utils::Optional<uint64_t>& rembMediaBps)
{
    uint32_t senderReportSsrcs[_outboundSsrcCounters.capacity()];
    uint32_t receiverReportSsrcs[_inboundSsrcCounters.capacity()];
    uint32_t activeSsrcs[_inboundSsrcCounters.capacity()];

    ReportContext reportContext;
    reportContext.senderReportSsrcs = utils::Span<uint32_t>(senderReportSsrcs, _outboundSsrcCounters.capacity());
    reportContext.receiverReportSsrcs = utils::Span<uint32_t>(receiverReportSsrcs, _inboundSsrcCounters.capacity());
    reportContext.activeSsrcs = utils::Span<uint32_t>(activeSsrcs, _inboundSsrcCounters.capacity());

    fillReportContext(reportContext, timestamp);

    if (reportContext.senderReportSsrcs.empty() &&
        reportContext.receiverReportSsrcs.size() <= reportContext.activeSsrcs.size() / 2 && !rembMediaBps.isSet())
    {
        return false;
    }

    const uint32_t receiveReportSsrc = _outboundSsrcCounters.size() > 0 ? _outboundSsrcCounters.begin()->first : 0;
    if (rembMediaBps.isSet())
    {
        const uint32_t rembSsrc =
            reportContext.senderReportSsrcs.empty() ? receiveReportSsrc : reportContext.senderReportSsrcs[0];

        buildRemb(reportContext, timestamp, rembSsrc, rembMediaBps.get());
    }

    const auto wallClock = utils::Time::toNtp(utils::Time::now());
    bool rembSent = sendSenderReports(reportContext, wallClock, timestamp);
    rembSent |= sendReceiveReports(reportContext, wallClock, timestamp, receiveReportSsrc);

    if (reportContext.rtcpPacket)
    {
        _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
    }

    return rembSent;
}

// always add REMB right after SR/RR when there is still space in the packet
void RtcpReportProducer::buildRemb(ReportContext& reportContext,
    const uint64_t timestamp,
    uint32_t senderSsrc,
    uint64_t mediaBps)
{
    assert(reportContext.rembPacket.getLength() == 0);

    auto& remb = rtp::RtcpRembFeedback::create(reportContext.rembPacket.get() + reportContext.rembPacket.getLength(),
        senderSsrc);

    remb.setBitrate(mediaBps);
    for (const auto ssrc : reportContext.activeSsrcs)
    {
        remb.addSsrc(ssrc);
    }

    reportContext.rembPacket.setLength(reportContext.rembPacket.getLength() + remb.header.size());
    assert(!memory::PacketPoolAllocator::isCorrupt(reportContext.rembPacket));
}

void RtcpReportProducer::fillReportContext(ReportContext& report, uint64_t timestamp)
{
    size_t senderReportCount = 0;
    size_t receiverReportCount = 0;
    size_t activeCount = 0;

    for (auto& it : _outboundSsrcCounters)
    {
        const auto remainingTime = it.second.timeToSenderReport(timestamp);
        if (remainingTime == 0)
        {
            report.senderReportSsrcs[senderReportCount++] = it.first;
        }
    }

    for (auto& it : _inboundSsrcCounters)
    {
        const auto remainingTime = it.second.timeToReceiveReport(timestamp);
        if (remainingTime == 0)
        {
            report.receiverReportSsrcs[receiverReportCount++] = it.first;
        }
        if (utils::Time::diffLE(it.second.getLastActive(), timestamp, 5 * utils::Time::sec))
        {
            report.activeSsrcs[activeCount++] = it.first;
        }
    }

    report.senderReportSsrcs = report.senderReportSsrcs.subSpan(0, senderReportCount);
    report.receiverReportSsrcs = report.receiverReportSsrcs.subSpan(0, receiverReportCount);
    report.activeSsrcs = report.activeSsrcs.subSpan(0, activeCount);
}

bool RtcpReportProducer::sendSenderReports(ReportContext& reportContext, uint64_t wallClock, int64_t timestamp)
{
    if (reportContext.senderReportSsrcs.empty())
    {
        // Ensure we will not try to send remb
        return false;
    }

    static constexpr uint64_t ntp32Tick = 0x10000u; // 1/65536 sec
    const size_t packetLimit = std::min(static_cast<size_t>(_config.mtu), memory::Packet::maxLength());
    bool rembSent = false;
    size_t rembSize = reportContext.rembPacket.getLength();

    size_t nextReportBlocksCount =
        std::min(reportContext.receiverReportSsrcs.size(), rtp::RtcpHeader::maxReportsBlocks());

    if (rembSize > 0)
    {
        // The assumption is this the 1st method called then rtcpPacket is still is a null pointer
        assert(!reportContext.rtcpPacket);
        size_t remainingSpace = packetLimit - rembSize - rtp::RtcpSenderReport::minimumSize();
        const size_t availableSlotsForRR = remainingSpace / sizeof(rtp::ReportBlock);
        nextReportBlocksCount = std::min(nextReportBlocksCount, availableSlotsForRR);
    }

    uint64_t wallClockNtpReport = wallClock;

    for (const auto ssrc : reportContext.senderReportSsrcs)
    {
        auto it = _outboundSsrcCounters.find(ssrc);
        if (it == _outboundSsrcCounters.end())
        {
            assert(false); // we should be alone on this context
            continue;
        }

        if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
        {
            logger::warn("No space available to send SR", _loggableId.c_str());
            return rembSent;
        }

        auto* senderReport =
            rtp::RtcpSenderReport::create(reportContext.rtcpPacket->get() + reportContext.rtcpPacket->getLength());
        senderReport->ssrc = it->first;
        it->second.fillInReport(*senderReport, timestamp, wallClockNtpReport);
        wallClockNtpReport += ntp32Tick;
        auto blocksToSend = removeAndGetLastFromReport(reportContext.receiverReportSsrcs, nextReportBlocksCount);
        for (auto itSsrc = blocksToSend.rbegin(); itSsrc != blocksToSend.rend(); ++itSsrc)
        {
            auto receiveIt = _inboundSsrcCounters.find(*itSsrc);
            if (receiveIt == _inboundSsrcCounters.end())
            {
                assert(false);
                continue;
            }
            auto& block = senderReport->addReportBlock(receiveIt->first);
            receiveIt->second.fillInReportBlock(timestamp, block, wallClock);
        }
        reportContext.rtcpPacket->setLength(reportContext.rtcpPacket->getLength() + senderReport->header.size());
        assert(!memory::PacketPoolAllocator::isCorrupt(*reportContext.rtcpPacket));

        if (rembSize > 0)
        {
            if (reportContext.rtcpPacket->getLength() + rembSize > packetLimit)
            {
                // This must never happen but for some reason it does. we will send the current packet
                // and add remb on 1s position
                assert(false);
                _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
                if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
                {
                    logger::warn("No space available to send SR", _loggableId.c_str());
                    return false;
                }
            }

            reportContext.rtcpPacket->append(reportContext.rembPacket);
            reportContext.rembPacket.setLength(0);
            rembSize = 0;
            rembSent = true;
        }

        assert(reportContext.rtcpPacket->getLength() <= packetLimit);

        // Ensure remb is sent in the after 1st SR
        const uint32_t spaceAvailable = packetLimit - reportContext.rtcpPacket->getLength();
        const uint32_t availableSlotsForRR =
            (spaceAvailable - rtp::RtcpSenderReport::minimumSize() - rembSize) / sizeof(rtp::ReportBlock);

        nextReportBlocksCount = std::min(reportContext.receiverReportSsrcs.size(), rtp::RtcpHeader::maxReportsBlocks());

        if (spaceAvailable < rtp::RtcpSenderReport::minimumSize() ||
            availableSlotsForRR < std::min(reportContext.receiverReportSsrcs.size(), size_t(4)))
        {
            _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
        }
        else if (nextReportBlocksCount > availableSlotsForRR)
        {
            nextReportBlocksCount = availableSlotsForRR;
        }
    }

    return rembSent;
}

bool RtcpReportProducer::sendReceiveReports(ReportContext& reportContext,
    uint64_t wallClock,
    int64_t timestamp,
    uint32_t receiveReportSsrc)
{
    const size_t packetLimit = std::min(static_cast<size_t>(_config.mtu), memory::Packet::maxLength());
    bool rembSent = false;
    size_t rembSize = reportContext.rembPacket.getLength();

    uint32_t nextReportBlocksCount =
        std::min(reportContext.receiverReportSsrcs.size(), rtp::RtcpHeader::maxReportsBlocks());
    if (rembSize > 0)
    {
        if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
        {
            logger::warn("No space available to send RR", _loggableId.c_str());
            return false;
        }

        if (reportContext.rtcpPacket->getLength() + rembSize + rtp::RtcpReceiverReport::minimumSize() > packetLimit)
        {
            // This must never happen. If it's not fit is because we have already some SR then REMB should it be
            // sent already. Buf if this happen because some bug. Let's flush the packet to network and ensure
            // we have enough space
            assert(false);
        }

        _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
        if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
        {
            logger::warn("No space available to send RR", _loggableId.c_str());
            return false;
        }

        const auto availableSpace =
            packetLimit - (reportContext.rtcpPacket->getLength() + rembSize + rtp::RtcpReceiverReport::minimumSize());
        const auto maxAvailableReportBlocks = static_cast<uint32_t>(availableSpace / sizeof(rtp::ReportBlock));
        nextReportBlocksCount = std::min(nextReportBlocksCount, maxAvailableReportBlocks);
    }

    while (reportContext.receiverReportSsrcs.size() || rembSize > 0)
    {
        assert(!reportContext.rtcpPacket || reportContext.rtcpPacket->getLength() <= packetLimit);

        // This only can be non-zero on first iteration. And it that case the nextReportBlocksCount was already
        // calculated to fir REMB
        if (rembSize == 0)
        {
            nextReportBlocksCount =
                std::min(reportContext.receiverReportSsrcs.size(), rtp::RtcpHeader::maxReportsBlocks());
            const auto spaceNeeded =
                rtp::RtcpReceiverReport::minimumSize() + nextReportBlocksCount * sizeof(rtp::ReportBlock);
            if (reportContext.rtcpPacket && reportContext.rtcpPacket->getLength() + spaceNeeded > packetLimit)
            {
                _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
            }
        }

        if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
        {
            logger::warn("No space available to send RR", _loggableId.c_str());
            return rembSent;
        }

        auto* receiverReport =
            rtp::RtcpReceiverReport::create(reportContext.rtcpPacket->get() + reportContext.rtcpPacket->getLength());
        receiverReport->ssrc = receiveReportSsrc;
        auto blocksToSend = removeAndGetLastFromReport(reportContext.receiverReportSsrcs, nextReportBlocksCount);
        for (auto itSsrc = blocksToSend.rbegin(); itSsrc != blocksToSend.rend(); ++itSsrc)
        {
            auto receiveIt = _inboundSsrcCounters.find(*itSsrc);
            if (receiveIt == _inboundSsrcCounters.end())
            {
                assert(false);
                continue;
            }
            auto& block = receiverReport->addReportBlock(receiveIt->first);
            receiveIt->second.fillInReportBlock(timestamp, block, wallClock);
        }

        reportContext.rtcpPacket->setLength(receiverReport->header.size());
        assert(!memory::PacketPoolAllocator::isCorrupt(*reportContext.rtcpPacket));

        if (rembSize > 0)
        {
            if (reportContext.rtcpPacket->getLength() + rembSize > packetLimit)
            {
                // This must never happen but for some reason it does. we will send the current packet
                // and add remb on 1s position
                assert(false);
                _rtcpSender.sendRtcp(std::move(reportContext.rtcpPacket), timestamp);
                if (!ensurePacket(reportContext.rtcpPacket, _rtcpPacketAllocator))
                {
                    logger::warn("No space available to send SR", _loggableId.c_str());
                    return false;
                }
            }

            reportContext.rtcpPacket->append(reportContext.rembPacket);
            reportContext.rembPacket.setLength(0);
            rembSize = 0;
            rembSent = true;
        }
    }

    return rembSent;
}