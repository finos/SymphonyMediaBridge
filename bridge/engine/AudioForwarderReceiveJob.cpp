#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineMixer.h"
#include "codec/OpusDecoder.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace bridge
{
const uint8_t PCM16x2 = 10;

void AudioForwarderReceiveJob::onPacketDecoded(const memory::Packet& opusPacket,
    const uint32_t bytesProduced,
    const int16_t* decodedData)
{
    auto decodedPacket = memory::makePacket(_allocator);
    const auto opusHeader = rtp::RtpHeader::fromPacket(opusPacket);
    std::memcpy(decodedPacket->get(), opusPacket.get(), opusHeader->headerLength());

    const auto rtpHeader = rtp::RtpHeader::fromPacket(*decodedPacket);
    rtpHeader->payloadType = PCM16x2;
    std::memcpy(rtpHeader->getPayload(), decodedData, bytesProduced);
    decodedPacket->setLength(rtpHeader->headerLength() + bytesProduced);

    _engineMixer.onMixerAudioRtpPacketDecoded(_sender, decodedPacket, _allocator);
}

bool AudioForwarderReceiveJob::decodeOpus(const memory::Packet& opusPacket)
{
    const auto rtpHeader = rtp::RtpHeader::fromPacket(opusPacket);
    if (!rtpHeader)
    {
        return false;
    }

    if (!_ssrcContext._opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "decodeOpus",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext._opusDecoder.reset(new codec::OpusDecoder());
    }

    codec::OpusDecoder& decoder = *_ssrcContext._opusDecoder;
    if (!decoder.isInitialized())
    {
        return false;
    }

    int16_t decodedData[memory::Packet::size / sizeof(int16_t)];

    uint32_t bytesProduced = 0;
    while (decoder.decode(rtpHeader->getPayload(),
        opusPacket.getLength() - rtpHeader->headerLength(),
        rtpHeader->sequenceNumber,
        memory::Packet::size,
        decodedData,
        bytesProduced))
    {
        if (bytesProduced > 0)
        {
            onPacketDecoded(opusPacket, bytesProduced, decodedData);
        }
    }

    return true;
}

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
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
            if (rtpHeaderExtension.id != _ssrcContext._audioLevelExtensionId)
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

    // TODO it is better if this job is run on the sender job queue so we do not have to sync this.
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
        if (!decodeOpus(*_packet))
        {
            logger::debug("failed to decode opus packet", _sender->getLoggableId().c_str());
        }
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
