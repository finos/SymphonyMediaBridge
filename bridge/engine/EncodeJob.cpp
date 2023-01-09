#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/AudioLevel.h"
#include "codec/G711codec.h"
#include "codec/Opus.h"
#include "codec/OpusEncoder.h"
#include "codec/PcmUtils.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"
namespace bridge
{

EncodeJob::EncodeJob(memory::UniqueAudioPacket packet,
    SsrcOutboundContext& outboundContext,
    transport::Transport& transport,
    const uint64_t rtpTimestamp)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _packet(std::move(packet)),
      _outboundContext(outboundContext),
      _transport(transport),
      _rtpTimestamp(rtpTimestamp)
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

    auto& targetFormat = _outboundContext.rtpMap;
    if (targetFormat.format == bridge::RtpMap::Format::OPUS)
    {
        if (!_outboundContext.opusEncoder)
        {
            _outboundContext.opusEncoder.reset(new codec::OpusEncoder());
        }

        auto opusPacket = memory::makeUniquePacket(_outboundContext.allocator);
        if (!opusPacket)
        {
            logger::error("failed to make packet for opus encoded data", "OpusEncodeJob");
            return;
        }

        auto opusHeader = rtp::RtpHeader::create(*opusPacket);

        rtp::RtpHeaderExtension extensionHead(opusHeader->getExtensionHeader());
        auto cursor = extensionHead.extensions().begin();
        if (_outboundContext.rtpMap.absSendTimeExtId.isSet())
        {
            rtp::GeneralExtension1Byteheader absSendTime(_outboundContext.rtpMap.absSendTimeExtId.get(), 3);
            extensionHead.addExtension(cursor, absSendTime);
        }
        if (_outboundContext.rtpMap.audioLevelExtId.isSet())
        {
            rtp::GeneralExtension1Byteheader audioLevel(_outboundContext.rtpMap.audioLevelExtId.get(), 1);
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

        const auto encodedBytes = _outboundContext.opusEncoder->encode(pcm16Data,
            frames,
            opusHeader->getPayload(),
            opusPacket->size - opusHeader->headerLength());

        if (encodedBytes <= 0)
        {
            logger::error("Failed to encode opus, %d", "OpusEncodeJob", encodedBytes);
            return;
        }

        opusPacket->setLength(opusHeader->headerLength() + encodedBytes);
        opusHeader->ssrc = _outboundContext.ssrc;
        opusHeader->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        opusHeader->sequenceNumber = ++_outboundContext.rewrite.lastSent.sequenceNumber & 0xFFFFu;
        opusHeader->payloadType = targetFormat.payloadType;
        _transport.protectAndSend(std::move(opusPacket));
    }
    else if (targetFormat.format == bridge::RtpMap::Format::PCMA || targetFormat.format == bridge::RtpMap::Format::PCMU)
    {
        if (!_outboundContext.resampler)
        {
            _outboundContext.resampler =
                codec::createPcmResampler(codec::Opus::sampleRate / codec::Opus::packetsPerSecond, 48000, 8000);
            if (!_outboundContext.resampler)
            {
                return;
            }
        }

        uint32_t payloadLength = _packet->getLength() - pcm16Header->headerLength();
        auto* pcm16Data = reinterpret_cast<int16_t*>(pcm16Header->getPayload());

        codec::makeMono(pcm16Data, payloadLength);
        payloadLength /= 2;
        payloadLength = _outboundContext.resampler->resample(pcm16Data, payloadLength, pcm16Data);

        auto encodedPacket = memory::makeUniquePacket(_outboundContext.allocator);
        auto outHeader = rtp::RtpHeader::create(*encodedPacket);
        if (targetFormat.format == bridge::RtpMap::Format::PCMA)
        {
            codec::PcmaCodec::encode(pcm16Data, outHeader->getPayload(), payloadLength);
            encodedPacket->setLength(outHeader->headerLength() + payloadLength);
        }
        else if (targetFormat.format == bridge::RtpMap::Format::PCMU)
        {
            codec::PcmuCodec::encode(pcm16Data, outHeader->getPayload(), payloadLength);
            encodedPacket->setLength(outHeader->headerLength() + payloadLength);
        }

        outHeader->ssrc = _outboundContext.ssrc;
        outHeader->timestamp = (_rtpTimestamp * 8llu) & 0xFFFFFFFFllu;
        outHeader->sequenceNumber = ++_outboundContext.rewrite.lastSent.sequenceNumber & 0xFFFFu;
        outHeader->payloadType = targetFormat.payloadType;
        _transport.protectAndSend(std::move(encodedPacket));
    }
    else
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat.format));
        return;
    }
}

} // namespace bridge
