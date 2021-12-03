#include "SetMaxMediaBitrateJob.h"
#include "memory/Packet.h"
#include "rtp/RtcpFeedback.h"
#include "transport/RtcTransport.h"

namespace bridge
{

SetMaxMediaBitrateJob::SetMaxMediaBitrateJob(transport::RtcTransport& transport,
    uint32_t reporterSsrc,
    uint32_t ssrc,
    uint32_t bitrateKbps,
    memory::PacketPoolAllocator& allocator)
    : CountedJob(transport.getJobCounter()),
      _transport(transport),
      _ssrc(ssrc),
      _reporterSsrc(reporterSsrc),
      _bitrate(bitrateKbps),
      _allocator(allocator)
{
}

void SetMaxMediaBitrateJob::run()
{
    auto* packet = memory::makePacket(_allocator);
    if (!packet)
    {
        return;
    }

    auto& tmmbr = rtp::RtcpTemporaryMaxMediaBitrate::create(packet->get(), _reporterSsrc);
    tmmbr.addEntry(_ssrc, _bitrate * 1000, 34);
    packet->setLength(tmmbr.size());
    _transport.protectAndSend(packet, _allocator);
}

} // namespace bridge
