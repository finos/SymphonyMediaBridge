#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineMixer.h"
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

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    ActiveMediaList& activeMediaList,
    const uint8_t silenceThresholdLevel,
    const bool hasMixedAudioStreams,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _activeMediaList(activeMediaList),
      _silenceThresholdLevel(silenceThresholdLevel),
      _hasMixedAudioStreams(hasMixedAudioStreams),
      _extendedSequenceNumber(extendedSequenceNumber)
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

    const auto rtpHeaderExtensions = rtpHeader->getExtensionHeader();
    if (rtpHeaderExtensions)
    {
        auto c9infoExtId = _ssrcContext.rtpMap.c9infoExtId.valueOr(0);
        auto audioLevelExtId = _ssrcContext.rtpMap.audioLevelExtId.valueOr(0);

        utils::Optional<uint8_t> audioLevel;
        utils::Optional<bool> isPtt;
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
            }
            else if (0 != audioLevelExtId && rtpHeaderExtension.getId() == audioLevelExtId)
            {
                audioLevel.set(rtpHeaderExtension.data[0] & 0x7F);
            }
        }

        bool silence = false;
        if (audioLevel.isSet())
        {
            silence = audioLevel.get() > _silenceThresholdLevel;
        }
        if (isPtt.isSet())
        {
            silence = !isPtt.get();
            if (!audioLevel.isSet())
            {
                // We need to 'fake' reasonable audio levels for the reasons.
                audioLevel.set(silence ? 90 : 25);
            }

            _engineMixer.mapSsrc2UserId(_ssrcContext.ssrc, c9UserId);
        }

        _activeMediaList.onNewAudioLevel(_packet->endpointIdHash,
            audioLevel.valueOr(120),
            isPtt.isSet() && isPtt.get());

        if (silence)
        {
            _ssrcContext.markNextPacket = true;
            return;
        }
    }

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

    if (_ssrcContext.markNextPacket)
    {
        rtpHeader->marker = 1;
        _ssrcContext.markNextPacket = false;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
