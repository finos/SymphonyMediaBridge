#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/AudioLevel.h"
#include "codec/Opus.h"
#include "codec/OpusEncoder.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

EncodeJob::EncodeJob(memory::AudioPacket* packet,
    memory::AudioPacketPoolAllocator& allocator,
    SsrcOutboundContext& outboundContext,
    transport::Transport& transport,
    const uint64_t rtpTimestamp,
    const int32_t audioLevelExtensionId,
    int32_t absSendTimeExtensionId)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _packet(packet),
      _audioPacketPoolAllocator(allocator),
      _outboundContext(outboundContext),
      _transport(transport),
      _rtpTimestamp(rtpTimestamp),
      _audioLevelExtensionId(audioLevelExtensionId),
      _absSendTimeExtensionId(absSendTimeExtensionId)

{
    assert(packet);
    assert(packet->getLength() > 0);
}

EncodeJob::~EncodeJob()
{
    if (_packet)
    {
        _audioPacketPoolAllocator.free(_packet);
        _packet = nullptr;
    }
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

        auto opusPacket = memory::makePacket(_outboundContext._allocator);
        if (!opusPacket)
        {
            logger::error("failed to make packet for opus encoded data", "OpusEncodeJob");
            return;
        }

        auto opusHeader = rtp::RtpHeader::create(opusPacket->get(), memory::Packet::size);

        rtp::RtpHeaderExtension extensionHead(opusHeader->getExtensionHeader());
        auto cursor = extensionHead.extensions().begin();
        if (_absSendTimeExtensionId)
        {
            rtp::GeneralExtension1Byteheader absSendTime;
            absSendTime.id = 3;
            absSendTime.setDataLength(3);
            extensionHead.addExtension(cursor, absSendTime);
        }
        if (_audioLevelExtensionId > 0)
        {
            rtp::GeneralExtension1Byteheader audioLevel;
            audioLevel.setDataLength(1);
            audioLevel.id = 1;
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
            _outboundContext._allocator.free(opusPacket);
            return;
        }

        opusPacket->setLength(opusHeader->headerLength() + encodedBytes);
        opusHeader->ssrc = _outboundContext._ssrc;
        opusHeader->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        opusHeader->sequenceNumber = _outboundContext._sequenceCounter++ & 0xFFFFu;
        opusHeader->payloadType = targetFormat._payloadType;
        _transport.protectAndSend(opusPacket, _outboundContext._allocator);
    }
    else
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat._format));
        return;
    }
}

} // namespace bridge
