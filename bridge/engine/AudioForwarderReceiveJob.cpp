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
        auto pcmPacket = memory::makePacket(_audioPacketAllocator, *_packet);
        if (!pcmPacket)
        {
            return;
        }
        auto rtpHeader = rtp::RtpHeader::fromPacket(*pcmPacket);
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        memcpy(rtpHeader->getPayload(), decodedData, decodedPayloadLength);
        pcmPacket->setLength(rtpHeader->headerLength() + decodedPayloadLength);

        _engineMixer.onMixerAudioRtpPacketDecoded(_sender, pcmPacket, _audioPacketAllocator);
        return;
    }

    logger::error("Unable to decode opus packet, error code %d", "OpusDecodeJob", decodedFrames);
}

void AudioForwarderReceiveJob::decodeOpus(const memory::Packet& opusPacket)
{
    if (!_ssrcContext._opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "OpusDecodeJob",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext._opusDecoder.reset(new codec::OpusDecoder());
    }

    codec::OpusDecoder& decoder = *_ssrcContext._opusDecoder;

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

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    memory::AudioPacketPoolAllocator& audioPacketAllocator,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    ActiveMediaList& activeMediaList,
    const int32_t silenceThresholdLevel,
    const bool hasMixedAudioStreams,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(packet),
      _allocator(allocator),
      _audioPacketAllocator(audioPacketAllocator),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _activeMediaList(activeMediaList),
      _silenceThresholdLevel(silenceThresholdLevel),
      _hasMixedAudioStreams(hasMixedAudioStreams),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

AudioForwarderReceiveJob::~AudioForwarderReceiveJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}

void AudioForwarderReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    const auto rtpHeaderExtensions = rtpHeader->getExtensionHeader();
    if (rtpHeaderExtensions)
    {
        int32_t audioLevel = -1;

        for (const auto& rtpHeaderExtension : rtpHeaderExtensions->extensions())
        {
            if (rtpHeaderExtension.getId() != _ssrcContext._audioLevelExtensionId)
            {
                continue;
            }

            audioLevel = rtpHeaderExtension.data[0] & 0x7F;
            break;
        }

        _activeMediaList.onNewAudioLevel(_sender->getEndpointIdHash(), audioLevel);

        if (audioLevel >= _silenceThresholdLevel)
        {
            _allocator.free(_packet);
            _packet = nullptr;
            _ssrcContext._markNextPacket = true;
            return;
        }
    }

    const auto oldRolloverCounter = _ssrcContext._lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u", "AudioForwarderReceiveJob", _ssrcContext._ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext._ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "AudioForwarderReceiveJob",
                _ssrcContext._ssrc,
                _engineMixer.getLoggableId().c_str());
            _allocator.free(_packet);
            _packet = nullptr;
            return;
        }
    }

    if (!_sender->unprotect(_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "AudioForwarderReceiveJob",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }
    _ssrcContext._lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;

    if (_hasMixedAudioStreams && _ssrcContext._rtpMap._format == bridge::RtpMap::Format::OPUS)
    {
        decodeOpus(*_packet);
    }

    if (_ssrcContext._markNextPacket)
    {
        rtpHeader->marker = 1;
        _ssrcContext._markNextPacket = false;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext._rtpMap._payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_sender, _packet, _allocator, _extendedSequenceNumber);

    _packet = nullptr;
}

} // namespace bridge
