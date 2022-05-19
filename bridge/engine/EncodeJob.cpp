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
    auto& targetFormat = _outboundContext.rtpMap;
    if (targetFormat._format != bridge::RtpMap::Format::OPUS && targetFormat._format != bridge::RtpMap::Format::PCMA &&
        targetFormat._format != bridge::RtpMap::Format::PCMU)
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat._format));
        return;
    }

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

        int16_t* pcm16Payload = reinterpret_cast<int16_t*>(pcm16Header->getPayload());

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
            _outboundContext._opusEncoder.reset(new codec::OpusEncoder());
        }

        const uint32_t payloadLength = _packet->getLength() - pcm16Header->headerLength();
        const size_t frames = payloadLength / EngineMixer::bytesPerSample / EngineMixer::channelsPerFrame;

        const auto encodedBytes = _outboundContext.opusEncoder->encode(pcm16Data,
            frames,
            outHeader->getPayload(),
            encodedPacket->size - outHeader->headerLength());

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
        if (!_outboundContext._resampler)
        {
            _outboundContext._resampler =
                codec::createPcmResampler(codec::Opus::sampleRate / codec::Opus::packetsPerSecond, 48000, 8000);
            if (!_outboundContext._resampler)
            {
                return;
            }
        }

        size_t payload16Length = _packet->getLength() - pcm16Header->headerLength();
        auto pcm16Payload = reinterpret_cast<int16_t*>(RtpHeader::getPayload(_packet->get()));
        codec::makeMono(pcm16Payload, payload16Length);
        payload16Length /= 2;
        payload16Length = _outboundContext._resampler->resample(pcm16Payload, payload16Length, pcm16Payload);

        if (targetFormat._format == bridge::RtpMap::Format::PCMA)
        {
            codec::PcmuCodec::encode(pcm16Payload, outHeader->getPayload(), payload16Length);
            encodedPacket->setLength(outHeader->headerLength() + payload16Length);
        }
        else if (targetFormat._format == bridge::RtpMap::Format::PCMU)
        {
            codec::PcmuCodec::encode(pcm16Payload, outHeader->getPayload(), payload16Length);
            encodedPacket->setLength(outHeader->headerLength() + payload16Length);
        }

        outHeader->ssrc = _outboundContext._ssrc;
        outHeader->timestamp = (_rtpTimestamp * 8llu) & 0xFFFFFFFFllu;
        outHeader->sequenceNumber = _outboundContext._sequenceCounter++ & 0xFFFFu;
        outHeader->payloadType = targetFormat._payloadType;
        _transport.protectAndSend(std::move(encodedPacket));
    }
    else
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat.format));
        return;
    }
}

} // namespace bridge
