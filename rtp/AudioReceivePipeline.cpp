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
    : _ssrc(0),
      _rtpFrequency(rtpFrequency),
      _samplesPerPacket(ptime * rtpFrequency / 1000),
      _buffer(maxPackets),
      _estimator(rtpFrequency),
      _audioLevelExtensionId(audioLevelExtensionId),
      _targetDelay(0),
      _pcmData(7 * 2 * _samplesPerPacket, 2 * rtpFrequency / 50),
      _receiveBox(memory::page::alignedSpace(sizeof(int16_t) * CHANNELS * _samplesPerPacket))
{
    _receiveBox.audio = reinterpret_cast<int16_t*>(memory::page::allocate(_receiveBox.audioBufferSize));
}

AudioReceivePipeline::~AudioReceivePipeline()
{
    memory::page::free(_receiveBox.audio, _receiveBox.audioBufferSize);
}

bool AudioReceivePipeline::updateTargetDelay(double delay)
{
    auto prev = _targetDelay;
    _targetDelay = (_estimator.getJitterMaxStable() + 1) * _rtpFrequency / 1000;

    if (_targetDelay > prev + _rtpFrequency / 100)
    {
        logger::info("%u jitter increase to %0.3fms",
            "AudioReceivePipeline",
            _ssrc,
            _estimator.getJitterMaxStable() + 1);
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
            logger::debug("%u Old opus packet sequence %u expected %u, discarding",
                "AudioReceivePipeline",
                _ssrc,
                extendedSequenceNumber,
                _decoder.getExpectedSequenceNumber());
            return 0;
        }

        logger::debug("%u Lost opus packet sequence %u expected %u, fec",
            "AudioReceivePipeline",
            _ssrc,
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
    if (samplesProduced < _samplesPerPacket / 2)
    {
        logger::warn("%u failed to decode opus %zu", "AudioReceivePipeline", _ssrc, samplesProduced);
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
            logger::debug("%u shrunk %u packets, eliminated samples %u, eliminated %u packets, JB %zu, TD %u",
                "AudioReceivePipeline",
                _ssrc,
                _metrics.shrunkPackets,
                _metrics.eliminatedSamples,
                _metrics.eliminatedPackets,
                _buffer.getRtpDelay() + _pcmData.size(),
                _targetDelay);
            _metrics.shrunkPackets = 0;
            _metrics.eliminatedSamples = 0;
            _metrics.eliminatedPackets = 0;
        }

        auto result = _pcmData.append(audioData, samples * CHANNELS);
        if (!result)
        {
            logger::warn("%u failed to append seq %u ts %u",
                "AudioReceivePipeline",
                _ssrc,
                header->sequenceNumber.get(),
                header->timestamp.get());
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
        _ssrc = header->ssrc.get();
        _targetDelay = 25 * _rtpFrequency / 1000;
        _buffer.add(std::move(packet));
        _head.rtpTimestamp = header->timestamp - _samplesPerPacket * 2;
        _head.timestamp = receiveTime;
        _head.extendedSequenceNumber = header->sequenceNumber;
        --_head.extendedSequenceNumber;
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
        logger::debug(
            "%u received late packet %u with delay of %0.2fms and pcm buffer is empty. JB %u Replay paused to "
            "avoid stuttering",
            "AudioReceivePipeline",
            _ssrc,
            extendedSequenceNumber,
            delay,
            _buffer.count());
    }
    else if (_jitterEmergency.counter > 0 && delayInRtpCycles < _buffer.getRtpDelay() + _samplesPerPacket)
    {
        logger::debug("%u continue after emergency pause %u packets. Recent packet delay %.02fms, JB lag %ums, JB %u",
            "AudioReceivePipeline",
            _ssrc,
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
    }
    process(receiveTime);

    return posted;
}

// Fetch audio and suppress pops after underruns as well as resume
size_t AudioReceivePipeline::fetchStereo(size_t sampleCount)
{
    codec::clearStereo(_receiveBox.audio, _samplesPerPacket);
    const auto currentSize = _pcmData.size();
    if (currentSize < sampleCount * 2)
    {
        ++_receiveBox.underrunCount;
        if (_receiveBox.underrunCount % 100 == 0)
        {
            logger::info("%u underrun %u, TD %ums",
                "AudioReceivePipeline",
                _ssrc,
                _receiveBox.underrunCount,
                _targetDelay * 1000 / _rtpFrequency);
        }
        if (currentSize > 0)
        {
            _pcmData.fetch(_receiveBox.audio, sampleCount * CHANNELS);
            codec::AudioLinearFade fader(currentSize / CHANNELS);
            codec::fadeOutStereo(_receiveBox.audio, currentSize / CHANNELS, fader);
            logger::debug("fade out, TD %ums", "AudioReceivePipeline", _targetDelay * 1000 / _rtpFrequency);
            _pcmData.popFront(sampleCount * 2);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        else if (_receiveBox.underrunCount == 1)
        {
            _pcmData.replay(_receiveBox.audio, sampleCount * CHANNELS);

            codec::sineTail(_receiveBox.audio, 250, 48000, sampleCount);
            codec::AudioLinearFade fader(sampleCount);
            codec::fadeOutStereo(_receiveBox.audio, sampleCount, fader);
            logger::debug("%u appended sine tail, TD %ums",
                "AudioReceivePipeline",
                _ssrc,
                _targetDelay * 1000 / _rtpFrequency);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        return 0;
    }

    _pcmData.fetch(_receiveBox.audio, sampleCount * CHANNELS);
    _pcmData.popFront(sampleCount * CHANNELS);
    if (_receiveBox.underrunCount > 0)
    {
        logger::debug("%u fade in", "AudioReceivePipeline", _ssrc);
        codec::AudioLinearFade fader(sampleCount);
        codec::fadeInStereo(_receiveBox.audio, sampleCount, fader);
        _receiveBox.underrunCount = 0;
        _receiveBox.audioSampleCount = sampleCount;
    }
    return sampleCount;
}

void AudioReceivePipeline::process(uint64_t timestamp)
{
    if (!_buffer.empty())
    {
        auto header = _buffer.getFrontRtp();
        const bool isNext = (header->sequenceNumber == ((_head.extendedSequenceNumber + 1) & 0xFFFFu));
        const bool isDue = (header->timestamp - _head.rtpTimestamp + _pcmData.size() / CHANNELS) <=
            (timestamp - _head.timestamp) * _rtpFrequency / utils::Time::sec;

        if (_jitterEmergency.counter > 0)
        {
            return;
        }

        const auto bufferLevel = _pcmData.size();
        if ((bufferLevel <= CHANNELS * _samplesPerPacket && isDue) ||
            (bufferLevel > 0 && bufferLevel <= CHANNELS * _samplesPerPacket && isNext))
        {
            if (!isNext)
            {
                logger::debug("%u gap %u -> %u",
                    "AudioReceivePipeline",
                    _ssrc,
                    _head.rtpTimestamp,
                    header->timestamp.get());
            }

            const int16_t seqDiff =
                header->sequenceNumber.get() - static_cast<uint16_t>(_head.extendedSequenceNumber & 0xFFFFu);
            const uint32_t extendedSequenceNumber = _head.extendedSequenceNumber + seqDiff;
            auto packet = _buffer.pop();

            int16_t audioData[_samplesPerPacket * 4 * CHANNELS];
            const size_t decodedSamples = decodePacket(extendedSequenceNumber, timestamp, *packet, audioData);
            compact(*packet, audioData, decodedSamples);

            const int32_t rtpTimestampAdv = header->timestamp - _head.rtpTimestamp;
            _head.rtpTimestamp = header->timestamp;
            _head.timestamp += rtpTimestampAdv * utils::Time::ms / (_rtpFrequency / 1000);
            _head.extendedSequenceNumber += seqDiff;
            const auto timeRemaining = utils::Time::diff(_head.timestamp, timestamp);
            if (timeRemaining < 0 || timeRemaining > utils::Time::sec)
            {
                _head.timestamp = timestamp;
            }

            if ((header->sequenceNumber.get() % 100) == 0 && !_buffer.empty())
            {
                auto tail = _buffer.getTailRtp();
                logger::debug("%u, lag %ums",
                    "AudioReceivePipeline",
                    _ssrc,
                    static_cast<uint32_t>(tail->timestamp.get() - _head.rtpTimestamp + _pcmData.size() / CHANNELS) *
                        1000 / _rtpFrequency);
            }
        }
    }
}

} // namespace rtp
