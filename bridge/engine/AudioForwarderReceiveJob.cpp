#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineMixer.h"
#include "codec/AudioLevel.h"
#include "codec/G711.h"
#include "codec/G711codec.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "codec/PcmUtils.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace bridge
{

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    ActiveMediaList& activeMediaList,
    const uint8_t silenceThresholdLevel,
    const bool hasMixedAudioStreams,
    const bool needAudioLevel,
    const uint32_t extendedSequenceNumber)
    : RtpForwarderReceiveBaseJob(std::move(packet), sender, engineMixer, ssrcContext, extendedSequenceNumber),
      _activeMediaList(activeMediaList),
      _silenceThresholdLevel(silenceThresholdLevel),
      _hasMixedAudioStreams(hasMixedAudioStreams),
      _needAudioLevel(needAudioLevel)
{
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

/*
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

void AudioForwarderReceiveJob::decodeOpus(const memory::Packet& opusPacket)
{
    if (!_ssrcContext.opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "OpusDecodeJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext.opusDecoder.reset(new codec::OpusDecoder());
    }

    codec::OpusDecoder& decoder = *_ssrcContext.opusDecoder;

    if (!decoder.isInitialized())
    {
        return;
    }

    uint8_t decodedData[memory::AudioPacket::size];
    auto rtpPacket = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpPacket)
    {
        return;
    }

    const uint32_t headerLength = rtpPacket->headerLength();
    const uint32_t payloadLength = _packet->getLength() - headerLength;
    auto payloadStart = rtpPacket->getPayload();

    if (decoder.hasDecoded() && _extendedSequenceNumber != decoder.getExpectedSequenceNumber())
    {
        const int32_t lossCount = static_cast<int32_t>(_extendedSequenceNumber - decoder.getExpectedSequenceNumber());
        if (lossCount <= 0)
        {
            logger::debug("Old opus packet sequence %u expected %u, discarding",
                "OpusDecodeJob",
                _extendedSequenceNumber,
                decoder.getExpectedSequenceNumber());
            return;
        }

        logger::debug("Lost opus packet sequence %u expected %u, fec",
            "OpusDecodeJob",
            _extendedSequenceNumber,
            decoder.getExpectedSequenceNumber());

        const auto concealCount = std::min(5u, _extendedSequenceNumber - decoder.getExpectedSequenceNumber() - 1);
        for (uint32_t i = 0; i < concealCount; ++i)
        {
            const auto decodedFrames = decoder.conceal(decodedData);
            onPacketDecoded(decodedFrames, decodedData);
        }

        const auto decodedFrames = decoder.conceal(payloadStart, payloadLength, decodedData);
        onPacketDecoded(decodedFrames, decodedData);
    }

    const auto framesInPacketBuffer =
        memory::AudioPacket::size / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;

    const auto decodedFrames =
        decoder.decode(_extendedSequenceNumber, payloadStart, payloadLength, decodedData, framesInPacketBuffer);
    onPacketDecoded(decodedFrames, decodedData);
}

void AudioForwarderReceiveJob::decodeG711(const memory::Packet& g711Packet)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(g711Packet);

    auto pcmPacket = memory::makeUniquePacket(_engineMixer.getAudioAllocator(), g711Packet);
    auto pcmHeader = rtp::RtpHeader::fromPacket(*pcmPacket);

    const auto sampleCount = g711Packet.getLength() - rtpHeader->headerLength();
    auto pcmPayload = reinterpret_cast<int16_t*>(pcmHeader->getPayload());
    if (rtpHeader->payloadType == codec::Pcma::payloadType)
    {
        codec::PcmaCodec::decode(rtpHeader->getPayload(), pcmPayload, sampleCount);
        pcmPacket->setLength(pcmHeader->headerLength() + sampleCount * sizeof(int16_t));
        // resample
        codec::makeStereo(pcmPayload, sampleCount);
    }
    else if (rtpHeader->payloadType == codec::Pcmu::payloadType)
    {
        codec::PcmuCodec::decode(rtpHeader->getPayload(), pcmPayload, sampleCount);
        // resample
        codec::makeStereo(pcmPayload, sampleCount);
        pcmPacket->setLength(pcmHeader->headerLength() + sampleCount * sizeof(int16_t) * 2);
    }
}
*/

int AudioForwarderReceiveJob::computeOpusAudioLevel(const memory::Packet& opusPacket)
{
    if (!_ssrcContext.opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s. %s",
            "AudioForwarderReceiveJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str(),
            _sender->getLoggableId().c_str());
        _ssrcContext.opusDecoder.reset(new codec::OpusDecoder());
        _ssrcContext.opusPacketRate.reset(new utils::AvgRateTracker(0.1));
    }

    const auto rtpHeader = rtp::RtpHeader::fromPacket(opusPacket);
    memory::AudioPacket pcmPacket;
    pcmPacket.append(_packet->get(), rtpHeader->headerLength());
    decode(opusPacket, pcmPacket);
    if (pcmPacket.getLength() == 0)
    {
        logger::warn("opus decode failed for ssrc %u", "AudioForwarderReceiveJob", _ssrcContext.ssrc);
        return -1;
    }

    _ssrcContext.opusPacketRate->update(1, utils::Time::getAbsoluteTime());
    return codec::computeAudioLevel(pcmPacket);
}

void AudioForwarderReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        return;
    }

    const bool isSsrcUsed = _ssrcContext.isSsrcUsed.load();

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
                if (_hasMixedAudioStreams && _ssrcContext.audioReceivePipe)
                {
                    _ssrcContext.audioReceivePipe->onSilencedRtpPacket(_extendedSequenceNumber,
                        memory::makeUniquePacket(_engineMixer.getMainAllocator(),
                            _packet->get(),
                            rtpHeader->headerLength()),
                        utils::Time::getAbsoluteTime());
                }
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
            logger::info("endpoint %zu does not send audio level RTP header extension. ssrc %u, %s ",
                "AudioForwarderReceiveJob",
                _sender->getEndpointIdHash(),
                _ssrcContext.ssrc,
                _sender->getLoggableId().c_str());
        }
        _ssrcContext.hasAudioLevelExtension = false;
    }

    if (!tryUnprotectRtpPacket("AudioForwarderReceiveJob"))
    {
        return;
    }

    int calculatedAudioLevel = -1;
    if (_ssrcContext.rtpMap.format == bridge::RtpMap::Format::OPUS)
    {
        if (_hasMixedAudioStreams)
        {
            if (!_ssrcContext.audioReceivePipe)
            {
                _ssrcContext.audioReceivePipe =
                    std::make_unique<codec::AudioReceivePipeline>(_ssrcContext.rtpMap.sampleRate,
                        20,
                        100,
                        _ssrcContext.rtpMap.audioLevelExtId.valueOr(255));
                _ssrcContext.hasAudioReceivePipe = true;
            }
            if (isSsrcUsed)
            {
                _ssrcContext.audioReceivePipe->onRtpPacket(_extendedSequenceNumber,
                    memory::makeUniquePacket(_engineMixer.getMainAllocator(), *_packet),
                    utils::Time::getAbsoluteTime());
            }
            else
            {
                _ssrcContext.audioReceivePipe->onSilencedRtpPacket(_extendedSequenceNumber,
                    memory::makeUniquePacket(_engineMixer.getMainAllocator(),
                        _packet->get(),
                        rtpHeader->headerLength()),
                    utils::Time::getAbsoluteTime());
            }
        }

        if (_needAudioLevel && !audioLevel.isSet())
        {
            calculatedAudioLevel = computeOpusAudioLevel(*_packet);
        }
        else if (_ssrcContext.opusPacketRate && _ssrcContext.opusPacketRate->get() != 0)
        {
            logger::debug("stop decoding opus audio level for %u. %s",
                "AudioForwarderReceiveJob",
                _ssrcContext.ssrc,
                _sender->getLoggableId().c_str());
            _ssrcContext.opusPacketRate->set(0, 0);
        }
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

    /*
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
            return;
        }
    }

    if (!_sender->unprotect(*_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "AudioForwarderReceiveJob",
            _ssrcContext.ssrc,
            _engineMixer.getLoggableId().c_str());
        return;
    }
    _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;

    if (_hasMixedAudioStreams && _ssrcContext.rtpMap.format == bridge::RtpMap::Format::OPUS)
    {
        decodeOpus(*_packet);
    }
    else if (_hasMixedAudioStreams &&
        (rtpHeader->payloadType == codec::Pcma::payloadType ||
            rtpHeader->payloadType == static_cast<uint16_t>(bridge::RtpMap::Format::PCMU)))
    {
        decodeG711(*_packet);
    }
    */

    if (_ssrcContext.markNextPacket && !silence)
    {
        rtpHeader->marker = 1;
        _ssrcContext.markNextPacket = false;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
