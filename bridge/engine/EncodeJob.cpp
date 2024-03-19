#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "codec/G711codec.h"
#include "codec/Opus.h"
#include "codec/OpusEncoder.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

using namespace bridge;

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
    if (targetFormat.format != bridge::RtpMap::Format::OPUS && targetFormat.format != bridge::RtpMap::Format::PCMA &&
        targetFormat.format != bridge::RtpMap::Format::PCMU)
    {
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat.format));
        return;
    }

    const auto pcm16Header = rtp::RtpHeader::fromPacket(*_packet);
    if (!pcm16Header)
    {
        return;
    }

    auto encodedPacket = memory::makeUniquePacket(_outboundContext.allocator);
    if (!encodedPacket)
    {
        logger::error("failed to make packet for encoded audio data", "EncodeJob");
        return;
    }

    auto* pcm16Data = reinterpret_cast<int16_t*>(pcm16Header->getPayload());
    uint32_t pcm16DataLength = _packet->getLength() - pcm16Header->headerLength();

    auto rtpHeader = rtp::RtpHeader::create(*encodedPacket);

    if (targetFormat.format == bridge::RtpMap::Format::OPUS)
    {

        if (!_outboundContext.opusEncoder)
        {
            _outboundContext.opusEncoder = std::make_unique<codec::OpusEncoder>();
        }

        rtp::RtpHeaderExtension extensionHead(rtpHeader->getExtensionHeader());
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
            rtpHeader->setExtensions(extensionHead);
            encodedPacket->setLength(rtpHeader->headerLength());
        }

        const size_t frames = pcm16DataLength / EngineMixer::bytesPerSample / EngineMixer::channelsPerFrame;

        const auto encodedBytes = _outboundContext.opusEncoder->encode(pcm16Data,
            frames,
            rtpHeader->getPayload(),
            encodedPacket->size - rtpHeader->headerLength());

        if (encodedBytes <= 0)
        {
            logger::error("Failed to encode opus, %d", "OpusEncodeJob", encodedBytes);
            return;
        }

        encodedPacket->setLength(rtpHeader->headerLength() + encodedBytes);
        rtpHeader->ssrc = _outboundContext.ssrc;
        rtpHeader->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        rtpHeader->sequenceNumber = ++_outboundContext.getSequenceNumberReference() & 0xFFFFu;
        rtpHeader->payloadType = targetFormat.payloadType;
        _transport.protectAndSend(std::move(encodedPacket));
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

        codec::makeMono(pcm16Data, pcm16DataLength);
        pcm16DataLength /= 2;
        pcm16DataLength = _outboundContext.resampler->resample(pcm16Data, pcm16DataLength, pcm16Data);

        if (targetFormat.format == bridge::RtpMap::Format::PCMA)
        {
            codec::PcmaCodec::encode(pcm16Data, rtpHeader->getPayload(), pcm16DataLength);
            encodedPacket->setLength(rtpHeader->headerLength() + pcm16DataLength);
        }
        else if (targetFormat.format == bridge::RtpMap::Format::PCMU)
        {
            codec::PcmuCodec::encode(pcm16Data, rtpHeader->getPayload(), pcm16DataLength);
            encodedPacket->setLength(rtpHeader->headerLength() + pcm16DataLength);
        }

        rtpHeader->ssrc = _outboundContext.ssrc;
        rtpHeader->timestamp = (_rtpTimestamp * 8llu) & 0xFFFFFFFFllu;
        rtpHeader->sequenceNumber = ++_outboundContext.getSequenceNumberReference() & 0xFFFFu;
        rtpHeader->payloadType = targetFormat.payloadType;
        _transport.protectAndSend(std::move(encodedPacket));
    }
}
