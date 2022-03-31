#include "test/integration/emulator/AudioSource.h"
#include "codec/AudioLevel.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
#include <cmath>

namespace emulator
{

AudioSource::AudioSource(memory::PacketPoolAllocator& allocator, uint32_t ssrc)
    : _ssrc(ssrc),
      _nextRelease(0),
      _allocator(allocator),
      _phase(0.3),
      _rtpTimestamp(101203),
      _sequenceCounter(100),
      _amplitude(0),
      _frequency(340.0)
{
}

memory::Packet* AudioSource::getPacket(uint64_t timestamp)
{
    if (timeToRelease(timestamp) > 0)
    {
        return nullptr;
    }

    if (_nextRelease == 0)
    {
        _nextRelease = timestamp;
    }
    _nextRelease += utils::Time::ms * 20;

    auto* packet = memory::makePacket(_allocator);
    assert(packet);
    if (!packet)
    {
        return nullptr;
    }

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    rtpHeader->payloadType = 111;
    rtpHeader->sequenceNumber = _sequenceCounter++;
    rtpHeader->ssrc = _ssrc;
    rtpHeader->timestamp = _rtpTimestamp;
    _rtpTimestamp += 960;

    const auto samplesPerPacket = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    int16_t audio[codec::Opus::channelsPerFrame * samplesPerPacket];

    for (uint64_t x = 0; x < samplesPerPacket; ++x)
    {
        audio[x * 2] = _amplitude * sin(_phase + x * 2 * M_PI * _frequency / codec::Opus::sampleRate);
        audio[x * 2 + 1] = 0;
    }
    _phase += samplesPerPacket * 2 * M_PI * _frequency / codec::Opus::sampleRate;

    rtp::RtpHeaderExtension extensionHead;
    auto cursor = extensionHead.extensions().begin();

    rtp::GeneralExtension1Byteheader absSendTime(3, 3);
    extensionHead.addExtension(cursor, absSendTime);

    rtp::GeneralExtension1Byteheader audioLevel(1, 1);
    audioLevel.data[0] = codec::computeAudioLevel(audio, samplesPerPacket);
    extensionHead.addExtension(cursor, audioLevel);
    rtpHeader->setExtensions(extensionHead);

    assert(rtpHeader->headerLength() == 24);
    const auto bytesEncoded = _encoder.encode(audio,
        samplesPerPacket,
        static_cast<unsigned char*>(rtpHeader->getPayload()),
        memory::Packet::size - rtpHeader->headerLength());
    if (bytesEncoded > 0)
    {
        packet->setLength(rtpHeader->headerLength() + bytesEncoded);
        return packet;
    }
    else
    {
        _allocator.free(packet);
        return nullptr;
    }
}

int64_t AudioSource::timeToRelease(uint64_t timestamp) const
{
    if (_nextRelease == 0)
    {
        return 0;
    }

    const auto remainingTime = utils::Time::diff(timestamp, _nextRelease);
    if (remainingTime > 0)
    {
        return remainingTime;
    }

    return 0;
}

} // namespace emulator
