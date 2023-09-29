#include "test/integration/emulator/AudioSource.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "codec/Opus.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
#include <cmath>

namespace emulator
{

AudioSource::AudioSource(memory::PacketPoolAllocator& allocator, uint32_t ssrc, Audio fakeAudio, uint32_t ptime)
    : _ssrc(ssrc),
      _nextRelease(0),
      _allocator(allocator),
      _phase(0.3),
      _rtpTimestamp(101203),
      _sequenceCounter(16384),
      _amplitude(0),
      _frequency(340.0),
      _ptime(ptime),
      _isPtt(PttState::NotSpecified),
      _useAudioLevel(true),
      _emulatedAudioType(fakeAudio),
      _pcm16File(nullptr),
      _packetCount(0)
{
}

AudioSource::~AudioSource()
{
    if (_pcm16File)
    {
        ::fclose(_pcm16File);
    }
}

memory::UniquePacket AudioSource::getPacket(uint64_t timestamp)
{
    const auto samplesPerPacket = codec::Opus::sampleRate * _ptime / 1000;
    if (timeToRelease(timestamp) > 0)
    {
        return nullptr;
    }

    if (_nextRelease == 0)
    {
        _nextRelease = timestamp;
    }
    _nextRelease += utils::Time::ms * _ptime;

    auto packet = memory::makeUniquePacket(_allocator);
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

    int16_t audio[codec::Opus::channelsPerFrame * samplesPerPacket];
    _rtpTimestamp += samplesPerPacket;

    if (_emulatedAudioType == Audio::Opus && !_pcm16File)
    {
        for (uint64_t x = 0; x < samplesPerPacket; ++x)
        {
            audio[x * 2] = _amplitude * sin(_phase + x * 2 * M_PI * _frequency / codec::Opus::sampleRate);
            audio[x * 2 + 1] = 0;
        }
        _phase += samplesPerPacket * 2 * M_PI * _frequency / codec::Opus::sampleRate;

        if (_packetCount++ < 150)
        {
            // create noise floor
            for (uint64_t x = 0; x < samplesPerPacket; ++x)
            {
                audio[x * 2] *= 0.01;
            }
        }
        if (_tonePattern.onRatio < 1.0)
        {
            if (_tonePattern.silenceCountDown <= -20)
            {
                const auto p = 2.0 * _tonePattern.onRatio * (rand() % 1000) * 0.001;
                _tonePattern.silenceCountDown = 20.0 * (1.0 / (p + 0.0001) - 1.0);
            }
            if (_tonePattern.silenceCountDown-- >= 0)
            {
                for (uint64_t x = 0; x < samplesPerPacket; ++x)
                {
                    audio[x * 2] *= 0.001;
                }
            }
        }
    }
    else if (_emulatedAudioType == Audio::Opus && _pcm16File)
    {
        auto readSamples = ::fread(audio, sizeof(int16_t), samplesPerPacket, _pcm16File);
        if (readSamples < samplesPerPacket)
        {
            ::rewind(_pcm16File);
            readSamples = ::fread(audio, sizeof(int16_t), samplesPerPacket, _pcm16File);
        }
        codec::makeStereo(audio, samplesPerPacket);
    }

    rtp::RtpHeaderExtension extensionHead;
    auto cursor = extensionHead.extensions().begin();

    rtp::GeneralExtension1Byteheader absSendTime(3, 3);
    extensionHead.addExtension(cursor, absSendTime);

    if (_useAudioLevel)
    {
        rtp::GeneralExtension1Byteheader audioLevel(1, 1);

        if (_emulatedAudioType == Audio::Muted)
        {
            audioLevel.data[0] = 127;
        }
        else if (_emulatedAudioType == Audio::Fake)
        {
            int16_t sample = _amplitude * 0.51;
            audioLevel.data[0] = codec::computeAudioLevel(&sample, 1);
        }
        else
        {
            audioLevel.data[0] = codec::computeAudioLevel(audio, samplesPerPacket);
        }
        extensionHead.addExtension(cursor, audioLevel);
    }

    if (PttState::NotSpecified != _isPtt)
    {
        rtp::GeneralExtension1Byteheader c9hdrExtension(8, 4);
        // Construct pseudo user-id (we need 24 bits only) from ssrc,
        *((uint32_t*)&c9hdrExtension.data[0]) = _ssrc;
        c9hdrExtension.data[3] = PttState::Set == _isPtt ? 0x80 : 0x00;
        extensionHead.addExtension(cursor, c9hdrExtension);
    }

    rtpHeader->setExtensions(extensionHead);

    if (_emulatedAudioType == Audio::Opus)
    {
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
            logger::error("failed to encode opus", "AudioSource");
        }
    }
    else
    {
        packet->setLength(rtpHeader->headerLength() + 97);
        return packet;
    }

    return memory::UniquePacket();
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

void AudioSource::setPtt(const PttState isPtt)
{
    _isPtt = isPtt;
}

void AudioSource::setUseAudioLevel(const bool useAudioLevel)
{
    _useAudioLevel = useAudioLevel;
}

bool AudioSource::openPcm16File(const char* filename)
{
    if (_pcm16File)
    {
        ::fclose(_pcm16File);
        _pcm16File = nullptr;
    }

    _pcm16File = ::fopen(filename, "r");
    return _pcm16File != nullptr;
}

void AudioSource::enableIntermittentTone(double onRatio)
{
    _tonePattern.onRatio = onRatio;
}
} // namespace emulator
