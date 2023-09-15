#include "rtp/AudioReceivePipeline.h"
#include "codec/AudioFader.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "memory/Allocator.h"

namespace rtp
{
const size_t CHANNELS = 2;

AudioReceivePipeline::AudioReceivePipeline(uint32_t rtpFrequency,
    uint32_t ptime,
    uint32_t maxPackets,
    int audioLevelExtensionId)
    : _rtpFrequency(rtpFrequency),
      _samplesPerPacket(ptime * rtpFrequency / 1000),
      _buffer(maxPackets),
      _estimator(rtpFrequency),
      _audioLevelExtensionId(audioLevelExtensionId),
      _targetDelay(0),
      _pcmData(7 * 2 * _samplesPerPacket, 2 * rtpFrequency / 50),
      _underrunCount(0),
      _audioBufferSize(memory::page::alignedSpace(sizeof(int16_t) * CHANNELS * rtpFrequency * ptime / 1000)),
      _audio(nullptr)

{
    _audio = reinterpret_cast<int16_t*>(memory::page::allocate(_audioBufferSize));
}

AudioReceivePipeline::~AudioReceivePipeline()
{
    memory::page::free(_audio, _audioBufferSize);
}

bool AudioReceivePipeline::updateTargetDelay(double delay)
{
    auto prev = _targetDelay;
    _targetDelay = (_estimator.getJitterMaxStable() + 1) * _rtpFrequency / 1000;

    if (_targetDelay > prev + _rtpFrequency / 100)
    {
        logger::info("jitter increase to %0.3fms", "AudioReceivePipeline", _estimator.getJitterMaxStable() + 1);
    }
    return true;
}

// produces up to 4 packets of stereo pcm data
size_t AudioReceivePipeline::decodePacket(uint32_t extendedSequenceNumber,
    const uint64_t timestamp,
    const memory::Packet& packet,
    int16_t* audioData)
{
    const auto header = RtpHeader::fromPacket(packet);
    const int16_t* originalAudioStart = audioData;

    if (_decoder.hasDecoded() && extendedSequenceNumber != _decoder.getExpectedSequenceNumber())
    {
        const int32_t lossCount = static_cast<int32_t>(extendedSequenceNumber - _decoder.getExpectedSequenceNumber());
        if (lossCount <= 0)
        {
            logger::debug("Old opus packet sequence %u expected %u, discarding",
                "AudioReceivePipeline",
                extendedSequenceNumber,
                _decoder.getExpectedSequenceNumber());
            return 0;
        }

        logger::debug("Lost opus packet sequence %u expected %u, fec",
            "AudioReceivePipeline",
            extendedSequenceNumber,
            _decoder.getExpectedSequenceNumber());

        const auto concealCount = std::min(2u, extendedSequenceNumber - _decoder.getExpectedSequenceNumber() - 1);
        for (uint32_t i = 0; concealCount > 1 && i < concealCount - 1; ++i)
        {
            const auto decodedFrames = _decoder.conceal(reinterpret_cast<uint8_t*>(audioData));
            if (decodedFrames > 0)
            {
                audioData += CHANNELS * decodedFrames;
            }
        }

        const auto opusPayloadLength = packet.getLength() - header->headerLength();
        const auto decodedFrames =
            _decoder.conceal(header->getPayload(), opusPayloadLength, reinterpret_cast<uint8_t*>(audioData));
        if (decodedFrames > 0)
        {
            audioData += CHANNELS * decodedFrames;
        }
    }

    const auto decodedFrames = _decoder.decode(extendedSequenceNumber,
        header->getPayload(),
        packet.getLength() - header->headerLength(),
        reinterpret_cast<uint8_t*>(audioData),
        _samplesPerPacket);

    if (decodedFrames > 0)
    {
        audioData += CHANNELS * decodedFrames;
    }
    size_t samplesProduced = (audioData - originalAudioStart) / CHANNELS;
    if (samplesProduced < _samplesPerPacket)
    {
        logger::warn("failed to decode opus %zu", "AudioReceivePipeline", samplesProduced);
        return samplesProduced;
    }

    return samplesProduced;
}

size_t AudioReceivePipeline::compact(const memory::Packet& packet, int16_t* audioData, size_t samples)
{
    int lvl = 0;
    if (!rtp::getAudioLevel(packet, _audioLevelExtensionId, lvl))
    {
        lvl = codec::computeAudioLevel(audioData, samples * CHANNELS);
    }
    _noiseFloor.update(lvl);

    const auto header = RtpHeader::fromPacket(packet);
    const uint32_t EXPECTED_REDUCTION_SAMPLES = 20;
    const auto currentLag = (!_buffer.empty() ? _buffer.getTailRtp()->timestamp - _head.rtpTimestamp : 0) +
        _pcmData.size() / CHANNELS + samples;
    bool mustCompact =
        !_buffer.empty() && !_pcmData.empty() && (currentLag > _targetDelay + EXPECTED_REDUCTION_SAMPLES);

    if (mustCompact)
    {
        if (lvl > _noiseFloor.getLevel() - 6 && _pcmData.size() > _samplesPerPacket &&
            currentLag > _targetDelay + _samplesPerPacket)
        {
            ++_metrics.eliminatedPackets;
            _metrics.eliminatedSamples += samples;
            return 0;
        }
        else
        {
            const auto newSampleCount = codec::compactStereoSlope(audioData, samples);
            if (newSampleCount < _samplesPerPacket)
            {
                ++_metrics.shrunkPackets;
            }
            _metrics.eliminatedSamples += _samplesPerPacket - newSampleCount;
            _pcmData.append(audioData, newSampleCount * CHANNELS);
            return newSampleCount;
        }
    }
    else
    {
        if (_metrics.eliminatedSamples > 0)
        {
            logger::debug("shrunk %u packets, eliminated samples %u, eliminated %u packets, JB %zu",
                "AudioReceivePipeline",
                _metrics.shrunkPackets,
                _metrics.eliminatedSamples,
                _metrics.eliminatedPackets,
                _buffer.getRtpDelay() + _pcmData.size());
            _metrics.shrunkPackets = 0;
            _metrics.eliminatedSamples = 0;
            _metrics.eliminatedPackets = 0;
        }

        auto result = _pcmData.append(audioData, samples * CHANNELS);
        if (!result)
        {
            logger::warn("failed to append seq %u ts %u", "", header->sequenceNumber.get(), header->timestamp.get());
        }
        return samples;
    }
}

bool AudioReceivePipeline::onRtpPacket(uint32_t extendedSequenceNumber,
    memory::UniquePacket packet,
    uint64_t receiveTime)
{
    auto header = RtpHeader::fromPacket(*packet);
    if (!header)
    {
        return false; // corrupt
    }

    const double delay = _estimator.update(receiveTime, header->timestamp);
    if (_targetDelay == 0)
    {
        _targetDelay = 25 * _rtpFrequency / 1000;
        _buffer.add(std::move(packet));
        _head.rtpTimestamp = header->timestamp - _samplesPerPacket * 2;
        _head.timestamp = receiveTime;
        return true;
    }

    const auto delayInRtpCycles = delay * _rtpFrequency / 1000;

    if (_targetDelay < delayInRtpCycles && delay > 20.0 && _pcmData.empty())
    {
        if (!_jitterEmergency.counter)
        {
            _jitterEmergency.sequenceStart = extendedSequenceNumber;
        }
        ++_jitterEmergency.counter;
        logger::debug("received late packet %u with delay of %0.2fms and pcm buffer is empty. JB %u Replay paused to "
                      "avoid stuttering",
            "AudioReceivePipeline",
            extendedSequenceNumber,
            delay,
            _buffer.count());
    }
    else if (_jitterEmergency.counter > 0 && delayInRtpCycles < _buffer.getRtpDelay() + _samplesPerPacket)
    {
        logger::debug("continue after emergency pause %u packets. Recent packet delay %.02fms, JB lag %ums, JB %u",
            "AudioReceivePipeline",
            extendedSequenceNumber - _jitterEmergency.sequenceStart,
            delay,
            _targetDelay * 1000 / _rtpFrequency,
            _buffer.count());
        _jitterEmergency.counter = 0;
    }

    const auto posted = _buffer.add(std::move(packet));
    if (posted)
    {
        updateTargetDelay(delay);
        _head.extendedSequenceNumber = extendedSequenceNumber;
    }
    process(receiveTime);

    return posted;
}

// Fetch audio and suppress pops after underruns as well as resume
size_t AudioReceivePipeline::fetchStereo(size_t sampleCount)
{
    codec::clearStereo(_audio, _samplesPerPacket);
    const auto currentSize = _pcmData.size();
    if (currentSize < sampleCount * 2)
    {
        ++_underrunCount;
        logger::info("underrun %u", "AudioReceivePipeline", _underrunCount);
        if (currentSize > 0)
        {
            _pcmData.fetch(_audio, sampleCount * CHANNELS);
            codec::AudioLinearFade fader(currentSize / CHANNELS);
            codec::fadeOutStereo(_audio, currentSize / CHANNELS, fader);
            logger::debug("fade out", "AudioReceivePipeline");
            _pcmData.popFront(sampleCount * 2);
            return sampleCount;
        }
        else if (_underrunCount == 1)
        {
            _pcmData.replay(_audio, sampleCount * CHANNELS);

            codec::sineTail(_audio, 250, 48000, sampleCount);
            codec::AudioLinearFade fader(sampleCount);
            codec::fadeOutStereo(_audio, sampleCount, fader);
            logger::debug("appended sine tail", "AudioReceivePipeline");
            return sampleCount;
        }
        return 0;
    }

    _pcmData.fetch(_audio, sampleCount * CHANNELS);
    _pcmData.popFront(sampleCount * CHANNELS);
    if (_underrunCount > 0)
    {
        logger::debug("fade in", "AudioReceivePipeline");
        codec::AudioLinearFade fader(sampleCount);
        codec::fadeInStereo(_audio, sampleCount, fader);
        _underrunCount = 0;
    }
    return sampleCount;
}

void AudioReceivePipeline::process(uint64_t timestamp)
{
    if (!_buffer.empty())
    {
        auto header = _buffer.getFrontRtp();
        const bool isNext = (header->timestamp == _head.rtpTimestamp);
        const bool isDue = (header->timestamp - _head.rtpTimestamp + _pcmData.size() / CHANNELS) <=
            (timestamp - _head.timestamp) * _rtpFrequency / utils::Time::sec;

        const auto bufferLevel = _pcmData.size();
        if (_jitterEmergency.counter > 0) {}
        else if ((bufferLevel <= CHANNELS * _samplesPerPacket && isDue) ||
            (bufferLevel > 0 && _pcmData.size() <= CHANNELS * _samplesPerPacket && isNext))
        {
            if (!isNext)
            {
                logger::debug("gap %u -> %u", "", _head.rtpTimestamp, header->timestamp.get());
            }

            if (_pcmData.empty())
            {
                _head.timestamp = timestamp;
                _head.rtpTimestamp = header->timestamp;
            }

            const int16_t seqDiff =
                header->sequenceNumber.get() - static_cast<uint16_t>(_head.extendedSequenceNumber & 0xFFFFu);
            const uint32_t extendedSequenceNumber = _head.extendedSequenceNumber + seqDiff;
            auto packet = _buffer.pop();

            int16_t audioData[_audioBufferSize * 4 / sizeof(int16_t)];
            const size_t decodedSamples = decodePacket(extendedSequenceNumber, timestamp, *packet, audioData);
            compact(*packet, audioData, decodedSamples);

            _head.rtpTimestamp = header->timestamp + _samplesPerPacket;
            _head.timestamp += utils::Time::ms * 20;

            if ((header->sequenceNumber.get() % 100) == 0 && !_buffer.empty())
            {
                auto tail = _buffer.getTailRtp();
                logger::debug("ssrc %u, lag %ums",
                    "AudioReceivePipeline",
                    header->ssrc.get(),
                    static_cast<uint32_t>(
                        tail->timestamp.get() - _head.rtpTimestamp + _samplesPerPacket + _pcmData.size() / CHANNELS) *
                        1000 / _rtpFrequency);
            }
        }
    }
}

} // namespace rtp
