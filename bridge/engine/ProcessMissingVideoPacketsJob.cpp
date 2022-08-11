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

void ProcessMissingVideoPacketsJob::run()
{
    if (!_ssrcContext.videoMissingPacketsTracker.get())
    {
        assert(false);
        return;
    }

    std::array<uint16_t, VideoMissingPacketsTracker::maxMissingPackets> missingSequenceNumbers;
    const auto numMissingSequenceNumbers =
        _ssrcContext.videoMissingPacketsTracker->process(utils::Time::getAbsoluteTime() / 1000000ULL,
            _ssrcContext.sender->getRtt() / utils::Time::ms,
            missingSequenceNumbers);

    if (numMissingSequenceNumbers == 0)
    {
        return;
    }

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
