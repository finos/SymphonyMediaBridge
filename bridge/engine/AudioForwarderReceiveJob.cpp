#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineMixer.h"
#include "codec/AudioLevel.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace bridge
{

void AudioForwarderReceiveJob::onPacketDecoded(const int32_t decodedFrames, const uint8_t* decodedData)
{
    if (decodedFrames > 0)
    {
        auto pcmPacket = memory::makeUniquePacket(_engineMixer.getAudioAllocator(), *_packet);
        if (!pcmPacket)
        {
            return;
        }
        auto rtpHeader = rtp::RtpHeader::fromPacket(*pcmPacket);
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        memcpy(rtpHeader->getPayload(), decodedData, decodedPayloadLength);
        pcmPacket->setLength(rtpHeader->headerLength() + decodedPayloadLength);

        _engineMixer.onMixerAudioRtpPacketDecoded(_ssrcContext, std::move(pcmPacket));
        return;
    }

    logger::error("Unable to decode opus packet, error code %d", "OpusDecodeJob", decodedFrames);
}

memory::UniqueAudioPacket AudioForwarderReceiveJob::makePcmPacket(const memory::Packet& opusPacket,
    uint32_t sequenceNumber)
{
    const auto opusRtpHeader = rtp::RtpHeader::fromPacket(opusPacket);
    auto pcmPacket =
        memory::makeUniquePacket(_engineMixer.getAudioAllocator(), opusPacket.get(), opusRtpHeader->headerLength());
    if (!pcmPacket)
    {
        return nullptr;
    }
    auto pcmRtpHeader = rtp::RtpHeader::fromPacket(*pcmPacket);
    pcmRtpHeader->sequenceNumber = sequenceNumber & 0xFFFFu;
    pcmRtpHeader->payloadType = 10;
    return pcmPacket;
}

void AudioForwarderReceiveJob::conceal(memory::AudioPacket& pcmPacket)
{
    codec::OpusDecoder& decoder = *_ssrcContext.opusDecoder;
    auto pcmHeader = rtp::RtpHeader::fromPacket(pcmPacket);
    const auto decodedFrames = decoder.conceal(pcmHeader->getPayload());
    if (decodedFrames > 0)
    {
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        pcmPacket.setLength(pcmHeader->headerLength() + decodedPayloadLength);
    }
    else
    {
        pcmPacket.setLength(0);
    }
}

void AudioForwarderReceiveJob::conceal(const memory::Packet& opusPacket, memory::AudioPacket& pcmPacket)
{
    codec::OpusDecoder& decoder = *_ssrcContext.opusDecoder;
    auto pcmHeader = rtp::RtpHeader::fromPacket(pcmPacket);
    const auto opusHeader = rtp::RtpHeader::fromPacket(opusPacket);
    const auto opusPayloadLength = opusPacket.getLength() - opusHeader->headerLength();
    const auto decodedFrames = decoder.conceal(opusHeader->getPayload(), opusPayloadLength, pcmHeader->getPayload());
    if (decodedFrames > 0)
    {
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        pcmPacket.setLength(pcmHeader->headerLength() + decodedPayloadLength);
    }
    else
    {
        pcmPacket.setLength(0);
    }
}

void AudioForwarderReceiveJob::decode(const memory::Packet& opusPacket, memory::AudioPacket& pcmPacket)
{
    const auto framesInPacketBuffer =
        memory::AudioPacket::size / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;

    codec::OpusDecoder& decoder = *_ssrcContext.opusDecoder;
    auto pcmHeader = rtp::RtpHeader::fromPacket(pcmPacket);
    const auto opusHeader = rtp::RtpHeader::fromPacket(opusPacket);
    const auto decodedFrames = decoder.decode(_extendedSequenceNumber,
        opusHeader->getPayload(),
        opusPacket.getLength() - opusHeader->headerLength(),
        pcmHeader->getPayload(),
        framesInPacketBuffer);

    if (decodedFrames > 0)
    {
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        pcmPacket.setLength(pcmHeader->headerLength() + decodedPayloadLength);
    }
    else
    {
        pcmPacket.setLength(0);
    }
}

bool AudioForwarderReceiveJob::unprotect(memory::Packet& opusPacket)
{
    const auto oldRolloverCounter = _ssrcContext.lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u", "AudioForwarderReceiveJob", _ssrcContext.ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext.ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "AudioForwarderReceiveJob",
                _ssrcContext.ssrc,
                _engineMixer.getLoggableId().c_str());
            return false;
        }
    }

    if (!_sender->unprotect(opusPacket))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "AudioForwarderReceiveJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        return false;
    }
    _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;

    return true;
}

// @return -1 on error, otherwise audio level if requested.
int AudioForwarderReceiveJob::decodeOpus(const memory::Packet& opusPacket, bool needAudioLevel)
{
    if (!_ssrcContext.opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "OpusDecodeJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext.opusDecoder.reset(new codec::OpusDecoder());
        _ssrcContext.opusPacketRate.reset(new utils::AvgRateTracker(0.1));
    }

    codec::OpusDecoder& decoder = *_ssrcContext.opusDecoder;

    if (!decoder.isInitialized())
    {
        return -1;
    }

    auto rtpPacket = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpPacket)
    {
        return -1;
    }

    if (decoder.hasDecoded() && _extendedSequenceNumber != decoder.getExpectedSequenceNumber())
    {
        const int32_t lossCount = static_cast<int32_t>(_extendedSequenceNumber - decoder.getExpectedSequenceNumber());
        if (lossCount <= 0)
        {
            logger::debug("Old opus packet sequence %u expected %u, discarding",
                "OpusDecodeJob",
                _extendedSequenceNumber,
                decoder.getExpectedSequenceNumber());
            return -1;
        }

        logger::debug("Lost opus packet sequence %u expected %u, fec",
            "OpusDecodeJob",
            _extendedSequenceNumber,
            decoder.getExpectedSequenceNumber());

        const auto concealCount = std::min(5u, _extendedSequenceNumber - decoder.getExpectedSequenceNumber() - 1);
        for (uint32_t i = 0; concealCount > 1 && i < concealCount - 1; ++i)
        {
            const uint32_t sequenceNumber = _extendedSequenceNumber - concealCount - 1 + i;
            auto pcmPacket = makePcmPacket(*_packet, sequenceNumber);
            if (!pcmPacket)
            {
                return -1;
            }
            conceal(*pcmPacket);
            if (pcmPacket->getLength() > 0)
            {
                _engineMixer.onMixerAudioRtpPacketDecoded(_ssrcContext, std::move(pcmPacket));
            }
        }

        auto pcmPacket = makePcmPacket(*_packet, _extendedSequenceNumber - 1);
        if (!pcmPacket)
        {
            return -1;
        }
        conceal(*_packet, *pcmPacket);
        if (pcmPacket->getLength() > 0)
        {
            _engineMixer.onMixerAudioRtpPacketDecoded(_ssrcContext, std::move(pcmPacket));
        }
    }

    auto pcmPacket = makePcmPacket(*_packet, _extendedSequenceNumber);
    if (!pcmPacket)
    {
        return -1;
    }
    decode(*_packet, *pcmPacket);
    if (pcmPacket->getLength() == 0)
    {
        return -1;
    }
    _ssrcContext.opusPacketRate->update(1, utils::Time::getAbsoluteTime());
    int audioLevel = 0;
    if (needAudioLevel)
    {
        audioLevel = codec::computeAudioLevel(*pcmPacket);
    }
    _engineMixer.onMixerAudioRtpPacketDecoded(_ssrcContext, std::move(pcmPacket));
    return audioLevel;
}

int AudioForwarderReceiveJob::computeOpusAudioLevel(const memory::Packet& opusPacket)
{
    if (!_ssrcContext.opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "OpusDecodeJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext.opusDecoder.reset(new codec::OpusDecoder());
        _ssrcContext.opusPacketRate.reset(new utils::AvgRateTracker(0.1));
    }

    const auto rtpHeader = rtp::RtpHeader::fromPacket(opusPacket);
    memory::AudioPacket pcmPacket;
    pcmPacket.append(_packet->get(), rtpHeader->headerLength());
    decode(opusPacket, pcmPacket);
    if (pcmPacket.getLength() == 0)
    {
        return -1;
    }
    _ssrcContext.opusPacketRate->update(1, utils::Time::getAbsoluteTime());
    return codec::computeAudioLevel(pcmPacket);
}

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    ActiveMediaList& activeMediaList,
    const uint8_t silenceThresholdLevel,
    const bool hasMixedAudioStreams,
    const bool needAudioLevel,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _activeMediaList(activeMediaList),
      _silenceThresholdLevel(silenceThresholdLevel),
      _hasMixedAudioStreams(hasMixedAudioStreams),
      _extendedSequenceNumber(extendedSequenceNumber),
      _needAudioLevel(needAudioLevel)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void AudioForwarderReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        return;
    }

    bool silence = false;
    utils::Optional<uint8_t> audioLevel;
    utils::Optional<bool> isPtt;
    const auto rtpHeaderExtensions = rtpHeader->getExtensionHeader();
    if (rtpHeaderExtensions)
    {
        auto c9infoExtId = _ssrcContext.rtpMap.c9infoExtId.valueOr(0);
        auto audioLevelExtId = _ssrcContext.rtpMap.audioLevelExtId.valueOr(0);

        uint32_t c9UserId = 0;

        for (const auto& rtpHeaderExtension : rtpHeaderExtensions->extensions())
        {
            if (0 != c9infoExtId && rtpHeaderExtension.getId() == c9infoExtId)
            {
                isPtt.set(rtpHeaderExtension.data[3] & 0x80);
                c9UserId = rtpHeaderExtension.data[0];
                c9UserId <<= 8;
                c9UserId |= rtpHeaderExtension.data[1];
                c9UserId <<= 8;
                c9UserId |= rtpHeaderExtension.data[2];
                _engineMixer.mapSsrc2UserId(_ssrcContext.ssrc, c9UserId);
            }
            else if (0 != audioLevelExtId && rtpHeaderExtension.getId() == audioLevelExtId)
            {
                audioLevel.set(rtpHeaderExtension.data[0] & 0x7F);
                silence = audioLevel.get() > _silenceThresholdLevel;
            }
        }
    }

    if (audioLevel.isSet())
    {
        _activeMediaList.onNewAudioLevel(_packet->endpointIdHash, audioLevel.get(), isPtt.isSet() && isPtt.get());

        if (silence)
        {
            if (_ssrcContext.markNextPacket)
            {
                return;
            }
            // Let first silent packet through to clients and barbells
            _ssrcContext.markNextPacket = true;
        }
    }
    else if (!_ssrcContext.opusDecoder)
    {
        // will touch the atomic only once. Reduces contention
        if (_ssrcContext.hasAudioLevelExtension.load())
        {
            logger::info("endpoint %zu does not send audio level RTP header extension. ssrc %u ",
                "AudioForwarderReceiveJob",
                _sender->getEndpointIdHash(),
                _ssrcContext.ssrc);
        }
        _ssrcContext.hasAudioLevelExtension = false;
    }

    if (!unprotect(*_packet))
    {
        return;
    }

    int calculatedAudioLevel = -1;
    if (_hasMixedAudioStreams && _ssrcContext.rtpMap.format == bridge::RtpMap::Format::OPUS)
    {
        calculatedAudioLevel = decodeOpus(*_packet, !audioLevel.isSet());
    }
    else if (_ssrcContext.rtpMap.format == bridge::RtpMap::Format::OPUS && _needAudioLevel && !audioLevel.isSet())
    {
        calculatedAudioLevel = computeOpusAudioLevel(*_packet);
    }

    if (!audioLevel.isSet())
    {
        if (calculatedAudioLevel < 0)
        {
            calculatedAudioLevel = 120;
        }
        _activeMediaList.onNewAudioLevel(_packet->endpointIdHash, calculatedAudioLevel, isPtt.isSet() && isPtt.get());
        silence = calculatedAudioLevel > _silenceThresholdLevel;
        if (_ssrcContext.rtpMap.audioLevelExtId.isSet())
        {
            rtp::addAudioLevel(*_packet, _ssrcContext.rtpMap.audioLevelExtId.get(), calculatedAudioLevel);
        }

        if (silence)
        {
            if (_ssrcContext.markNextPacket)
            {
                return;
            }
            // Let first silent packet through to clients and barbells
            _ssrcContext.markNextPacket = true;
        }
    }

    if (_ssrcContext.markNextPacket && !silence)
    {
        rtpHeader->marker = 1;
        _ssrcContext.markNextPacket = false;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
