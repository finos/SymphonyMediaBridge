#include "bridge/engine/ProcessMissingVideoPacketsJob.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "logger/Logger.h"
#include "rtp/RtcpNackBuilder.h"
#include "transport/RtcTransport.h"
#include "utils/Time.h"
#include <array>

namespace bridge
{

ProcessMissingVideoPacketsJob::ProcessMissingVideoPacketsJob(SsrcInboundContext& ssrcContext,
    const uint32_t reporterSsrc,
    transport::RtcTransport& transport,
    memory::PacketPoolAllocator& allocator)
    : CountedJob(transport.getJobCounter()),
      _ssrcContext(ssrcContext),
      _reporterSsrc(reporterSsrc),
      _transport(transport),
      _allocator(allocator)
{
}

// We check existence of packet tracker here because we run on the transport thread context, which is the context
// that also adds the packet tracker
void ProcessMissingVideoPacketsJob::run()
{
    auto timestamp = utils::Time::getAbsoluteTime();
    if (!_ssrcContext.videoMissingPacketsTracker)
    {
        return;
    }

    std::array<uint16_t, VideoMissingPacketsTracker::maxMissingPackets> missingSequenceNumbers;
    const auto numMissingSequenceNumbers = _ssrcContext.videoMissingPacketsTracker->process(timestamp,
        _ssrcContext.sender->getRtt(),
        missingSequenceNumbers);

    if (numMissingSequenceNumbers == 0)
    {
        return;
    }

    logger::debug("send NACK for %zu pkts, ssrc %u, %s",
        "ProcessMissingVideoPacketsJob",
        numMissingSequenceNumbers,
        _ssrcContext.ssrc,
        _transport.getLoggableId().c_str());

    rtp::RtcpNackBuilder rtcpNackBuilder(_reporterSsrc, _ssrcContext.ssrc);

    for (size_t i = 0; i < numMissingSequenceNumbers; ++i)
    {
        if (!rtcpNackBuilder.appendSequenceNumber(missingSequenceNumbers[i]))
        {
            logger::debug("To many missing packets for one nack %lu",
                "ProcessMissingVideoPacketsJob",
                numMissingSequenceNumbers);
            break;
        }
    }

    size_t rtcpNackSize = 0;
    const auto rtcpNack = rtcpNackBuilder.build(rtcpNackSize);
    if (rtcpNackSize == 0)
    {
        return;
    }

    auto packet = memory::makeUniquePacket(_allocator, rtcpNack, rtcpNackSize);
    if (!packet)
    {
        return;
    }

    _transport.protectAndSend(std::move(packet));
}

} // namespace bridge
