#include "bridge/engine/SendRtpPaddingJob.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/ForwarderSendJob.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "logger/Logger.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace
{

const uint32_t maxPaddingBytesPerPacket = 255;

}

namespace bridge
{

SendRtpPaddingJob::SendRtpPaddingJob(SsrcOutboundContext& ssrcOutboundContext,
    transport::RtcTransport& transport,
    const uint32_t ssrc,
    const uint32_t numPackets,
    const uint32_t intervalMs,
    const utils::Optional<uint8_t>& absSendTimeExtensionId,
    const uint64_t rtpTimestamp,
    const uint32_t iterationsLeft,
    const uint32_t firstSequenceNumber,
    const uint32_t targetBitrateKbps)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _ssrcOutboundContext(ssrcOutboundContext),
      _transport(transport),
      _ssrc(ssrc),
      _numPackets(numPackets),
      _intervalMs(intervalMs),
      _absSendTimeExtensionId(absSendTimeExtensionId),
      _rtpTimestamp(rtpTimestamp),
      _iterationsLeft(iterationsLeft),
      _firstSequenceNumber(firstSequenceNumber),
      _targetBitrateKbps(targetBitrateKbps)
{
}

void SendRtpPaddingJob::run()
{
    if (!_transport.isConnected() || _ssrcOutboundContext._markedForDeletion)
    {
        return;
    }

    for (uint32_t i = 0; i < _numPackets; ++i)
    {
        auto packet = memory::makePacket(_ssrcOutboundContext._allocator);
        if (!packet)
        {
            return;
        }

        auto rtpHeader = rtp::RtpHeader::create(packet->get(), memory::Packet::size);
        rtpHeader->ssrc = _ssrc;
        rtpHeader->payloadType = static_cast<uint16_t>(RtpMap::Format::VP8);
        rtpHeader->padding = 1;
        rtpHeader->marker = 0;
        rtpHeader->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        rtpHeader->sequenceNumber = (_firstSequenceNumber + i) & 0xFFFF;

        if (_absSendTimeExtensionId.isSet())
        {
            rtp::RtpHeaderExtension extensionHead(rtpHeader->getExtensionHeader());
            rtp::GeneralExtension1Byteheader absSendTime;
            absSendTime.id = _absSendTimeExtensionId.get();
            absSendTime.setDataLength(3);
            auto cursor = extensionHead.extensions().begin();
            extensionHead.addExtension(cursor, absSendTime);
            rtpHeader->setExtensions(extensionHead);
        }

        auto payloadStart = rtpHeader->getPayload();
        memset(payloadStart, 0, maxPaddingBytesPerPacket);
        payloadStart[maxPaddingBytesPerPacket - 1] = utils::checkedCast<uint8_t>(maxPaddingBytesPerPacket);
        packet->setLength(rtpHeader->headerLength() + maxPaddingBytesPerPacket);

        _transport.getJobManager().addJob<ForwarderSendJob>(_ssrcOutboundContext,
            packet,
            _firstSequenceNumber + i,
            _transport);
    }

    const auto uplinkEstimateKbps = _transport.getUplinkEstimateKbps();
    if (_iterationsLeft == 0 || uplinkEstimateKbps >= _targetBitrateKbps)
    {
        logger::info(
            "Done sending RTP padding, ssrc %u, transport %s, iterationsLeft %u, uplinkEstimateKbps %u, rtt %" PRIu64,
            "SendRtpPaddingJob",
            _ssrc,
            _transport.getLoggableId().c_str(),
            _iterationsLeft,
            uplinkEstimateKbps,
            _transport.getRtt() / utils::Time::ms);

        _ssrcOutboundContext._isSendingRtpPadding = false;
        return;
    }

    _transport.getJobManager().getJobManager().addTimedJob<bridge::SendRtpPaddingJob>(_transport.getId(),
        _ssrc,
        static_cast<uint64_t>(_intervalMs) * 1000ULL,
        _ssrcOutboundContext,
        _transport,
        _ssrc,
        _numPackets,
        _intervalMs,
        _absSendTimeExtensionId,
        _rtpTimestamp + static_cast<uint64_t>(_intervalMs),
        _iterationsLeft - 1,
        _firstSequenceNumber + _numPackets,
        _targetBitrateKbps);
}

} // namespace bridge
