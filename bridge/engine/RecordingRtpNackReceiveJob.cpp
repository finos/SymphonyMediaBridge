#include "bridge/engine/RecordingRtpNackReceiveJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "memory/RefCountedPacket.h"
#include "rtp/RtpHeader.h"
#include "transport/RecordingTransport.h"
#include "transport/recp/RecControlHeader.h"

#define DEBUG_REC_NACK_EVENT 0

namespace bridge
{
RecordingRtpNackReceiveJob::RecordingRtpNackReceiveJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    transport::RecordingTransport* sender,
    SsrcOutboundContext& ssrcOutboundContext)
    : CountedJob(sender->getJobCounter()),
      _packet(packet),
      _allocator(allocator),
      _sender(sender),
      _ssrcOutboundContext(ssrcOutboundContext)
{
}

RecordingRtpNackReceiveJob::~RecordingRtpNackReceiveJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}

void RecordingRtpNackReceiveJob::run()
{
    auto recControlHeader = recp::RecControlHeader::fromPacket(*_packet);
    if (!recControlHeader)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    if (!_ssrcOutboundContext._packetCache.isSet() || !_ssrcOutboundContext._packetCache.get())
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    auto packetCache = _ssrcOutboundContext._packetCache.get();
    auto scopedRef = memory::RefCountedPacket::ScopedRef(packetCache->get(recControlHeader->sequenceNumber));
    if (!scopedRef._refCountedPacket)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }
    auto cachedPacket = scopedRef._refCountedPacket->get();
    if (!cachedPacket)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    if (!rtp::isRtpPacket(cachedPacket->get(), cachedPacket->getLength()))
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    auto packet = memory::makePacket(_allocator);
    if (!packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    memcpy(packet->get(), cachedPacket->get(), cachedPacket->getLength());
    packet->setLength(cachedPacket->getLength());

#if DEBUG_REC_NACK_EVENT
    logger::debug("Sending NACKed packet for ssrc %u sequence %u",
        "RecordingRtpNackReceiveJob",
        _ssrcOutboundContext._ssrc,
        recControlHeader->sequenceNumber.get());
#endif

    _sender->protectAndSend(packet, _allocator);
    _allocator.free(_packet);
    _packet = nullptr;
}

} // namespace bridge
