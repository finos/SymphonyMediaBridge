#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/AudioLevel.h"
#include "codec/OpusEncoder.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

EncodeJob::EncodeJob(memory::AudioPacketPtr packet,
    SsrcOutboundContext& outboundContext,
    transport::Transport& transport,
    uint64_t rtpTimestamp,
    uint8_t audioLevelExtensionId,
    uint8_t absSendTimeExtensionId)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _packet(std::move(packet)),
      _outboundContext(outboundContext),
      _transport(transport),
      _rtpTimestamp(rtpTimestamp),
      _audioLevelExtensionId(audioLevelExtensionId),
      _absSendTimeExtensionId(absSendTimeExtensionId)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void EncodeJob::run()
{
    const auto pcm16Header = rtp::RtpHeader::fromPacket(*_packet);
    if (!pcm16Header)
    {
        return;
    }

    auto& targetFormat = _outboundContext._rtpMap;
    if (targetFormat._format == bridge::RtpMap::Format::OPUS)
    {
        if (!_outboundContext._opusEncoder)
        {
            _outboundContext._opusEncoder.reset(new codec::OpusEncoder());
        }

        auto opusPacket = memory::makePacketPtr(_outboundContext._allocator);
        if (!opusPacket)
        {
            logger::error("failed to make packet for opus encoded data", "OpusEncodeJob");
            return;
        }

        auto opusHeader = rtp::RtpHeader::create(*opusPacket);

        rtp::RtpHeaderExtension extensionHead(opusHeader->getExtensionHeader());
        auto cursor = extensionHead.extensions().begin();
        if (_absSendTimeExtensionId)
        {
            rtp::GeneralExtension1Byteheader absSendTime(_absSendTimeExtensionId, 3);
            extensionHead.addExtension(cursor, absSendTime);
        }
        if (_audioLevelExtensionId)
        {
            rtp::GeneralExtension1Byteheader audioLevel(_audioLevelExtensionId, 1);
            audioLevel.data[0] = codec::computeAudioLevel(*_packet);
            extensionHead.addExtension(cursor, audioLevel);
        }
        if (!extensionHead.empty())
        {
            opusHeader->setExtensions(extensionHead);
            opusPacket->setLength(opusHeader->headerLength());
        }

        const uint32_t payloadLength = _packet->getLength() - pcm16Header->headerLength();
        const size_t frames = payloadLength / EngineMixer::bytesPerSample / EngineMixer::channelsPerFrame;
        const auto* pcm16Data = reinterpret_cast<int16_t*>(pcm16Header->getPayload());

        const auto encodedBytes = _outboundContext._opusEncoder->encode(pcm16Data,
            frames,
            opusHeader->getPayload(),
            opusPacket->size - opusHeader->headerLength());

        if (encodedBytes <= 0)
        {
            logger::error("Failed to encode opus, %d", "OpusEncodeJob", encodedBytes);
            return;
        }

        opusPacket->setLength(opusHeader->headerLength() + encodedBytes);
        opusHeader->ssrc = _outboundContext._ssrc;
        opusHeader->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        opusHeader->sequenceNumber = _outboundContext._sequenceCounter++ & 0xFFFFu;
        opusHeader->payloadType = targetFormat._payloadType;
        _transport.protectAndSend(std::move(opusPacket));
    }
    else
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat._format));
        return;
    }
}

} // namespace bridge
