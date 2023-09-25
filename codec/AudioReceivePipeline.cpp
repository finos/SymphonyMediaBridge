#include "codec/AudioReceivePipeline.h"
#include "codec/AudioFader.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "math/helpers.h"
#include "memory/Allocator.h"

namespace codec
{
const size_t CHANNELS = 2;

AudioReceivePipeline::ReceiveBox::ReceiveBox(size_t bufferSize)
    : underrunCount(0),
      audioBufferSize(bufferSize),
      audio(nullptr),
      audioSampleCount(0)
{
    audio = reinterpret_cast<int16_t*>(memory::page::allocate(audioBufferSize));
}

AudioReceivePipeline::ReceiveBox::~ReceiveBox()
{
    memory::page::free(audio, audioBufferSize);
}

AudioReceivePipeline::AudioReceivePipeline(uint32_t rtpFrequency,
    uint32_t ptime,
    uint32_t maxPackets,
    int audioLevelExtensionId)
    : _ssrc(0),
      _rtpFrequency(rtpFrequency),
      _samplesPerPacket(ptime * rtpFrequency / 1000),
      _estimator(rtpFrequency),
      _audioLevelExtensionId(audioLevelExtensionId),
      _targetDelay(0),
      _pcmData(7 * CHANNELS * _samplesPerPacket, CHANNELS * _samplesPerPacket),
      _receiveBox(memory::page::alignedSpace(sizeof(int16_t) * CHANNELS * _samplesPerPacket))
{
}

bool AudioReceivePipeline::updateTargetDelay(double delay)
{
    const auto decodeTime = 3;

    auto prev = _targetDelay;

    _targetDelay = (_estimator.getJitterMaxStable() + 1 + decodeTime) * _rtpFrequency / 1000;

    if (_targetDelay > prev + _rtpFrequency / 100)
    {
        logger::info("%u jitter increase to %0.3fms",
            "AudioReceivePipeline",
            _ssrc,
            _targetDelay * 1000.0 / _rtpFrequency);
    }

    return true;
}

// produces up to 4 packets of stereo pcm data
size_t AudioReceivePipeline::decodePacket(uint32_t extendedSequenceNumber,
    const uint64_t timestamp,
    const memory::Packet& packet,
    int16_t* audioData)
{
    const auto header = rtp::RtpHeader::fromPacket(packet);
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

size_t AudioReceivePipeline::compact(const memory::Packet& packet, int16_t* audioData, uint32_t samples)
{
    int audioLevel = 0;
    if (!rtp::getAudioLevel(packet, _audioLevelExtensionId, audioLevel))
    {
        audioLevel = codec::computeAudioLevel(audioData, samples * CHANNELS);
    }
    _noiseFloor.update(audioLevel);

    const auto maxReduction =
        static_cast<int32_t>(totalBufferSize() - math::roundUpMultiple(_targetDelay, _samplesPerPacket));

    if (!_jitterBuffer.empty() && maxReduction > 10)
    {
        const auto bufferLevel = _pcmData.size() / CHANNELS;
        if (audioLevel > _noiseFloor.getLevel() - 6 && bufferLevel > _samplesPerPacket &&
            maxReduction >= static_cast<int32_t>(_samplesPerPacket))
        {
            ++_metrics.eliminatedPackets;
            _metrics.eliminatedSamples += samples;
            return 0;
        }
        else
        {
            const auto newSampleCount = codec::compactStereoTroughs(audioData, samples, maxReduction);
            if (newSampleCount < samples)
            {
                ++_metrics.shrunkPackets;
            }
            _metrics.eliminatedSamples += samples - newSampleCount;
            _pcmData.append(audioData, newSampleCount * CHANNELS);
            return newSampleCount;
        }
    }
    else
    {
        auto result = _pcmData.append(audioData, samples * CHANNELS);
        if (!result)
        {
            const auto header = rtp::RtpHeader::fromPacket(packet);
            logger::warn("%u failed to append seq %u ts %u",
                "AudioReceivePipeline",
                _ssrc,
                header->sequenceNumber.get(),
                header->timestamp.get());
        }
        if (_metrics.eliminatedSamples > 0)
        {
            logger::debug("%u shrunk %u packets, eliminated samples %u, eliminated %u packets, pcm %zu, JB %u, TD %u, "
                          "slowJitter %.2f",
                "AudioReceivePipeline",
                _ssrc,
                _metrics.shrunkPackets,
                _metrics.eliminatedSamples,
                _metrics.eliminatedPackets,
                _pcmData.size() / CHANNELS,
                totalBufferSize(),
                _targetDelay,
                _estimator.getJitterMaxStable());
            _metrics.shrunkPackets = 0;
            _metrics.eliminatedSamples = 0;
            _metrics.eliminatedPackets = 0;
        }
        return samples;
    }
}

bool AudioReceivePipeline::onRtpPacket(uint32_t extendedSequenceNumber,
    memory::UniquePacket packet,
    uint64_t receiveTime,
    bool isSsrcUsed)
{
    auto header = rtp::RtpHeader::fromPacket(*packet);
    if (!header)
    {
        return false; // corrupt
    }

    const double delay = _estimator.update(receiveTime, header->timestamp);
    if (_targetDelay == 0)
    {
        _ssrc = header->ssrc.get();
        _targetDelay = 25 * _rtpFrequency / 1000;
        _jitterBuffer.add(std::move(packet));
        _head.rtpTimestamp = header->timestamp;
        _head.extendedSequenceNumber = header->sequenceNumber - 1;
        _pcmData.appendSilence(_samplesPerPacket);
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
            _jitterBuffer.count());
    }
    else if (_jitterEmergency.counter > 0 && delayInRtpCycles < _jitterBuffer.getRtpDelay() + _samplesPerPacket)
    {
        logger::debug("%u continue after emergency pause %u packets. Recent packet delay %.02fms, JB lag %ums, JB %u",
            "AudioReceivePipeline",
            _ssrc,
            extendedSequenceNumber - _jitterEmergency.sequenceStart,
            delay,
            _targetDelay * 1000 / _rtpFrequency,
            _jitterBuffer.count());
        _jitterEmergency.counter = 0;
    }

    const auto posted = _jitterBuffer.add(std::move(packet));
    if (posted)
    {
        updateTargetDelay(delay);
    }

    process(receiveTime, isSsrcUsed);

    return posted;
}

// Fetch audio and suppress pops after underruns as well as resume
size_t AudioReceivePipeline::fetchStereo(size_t sampleCount)
{
    codec::clearStereo(_receiveBox.audio, _samplesPerPacket);
    const auto bufferLevel = _pcmData.size() / CHANNELS;
    if (bufferLevel < sampleCount)
    {
        ++_receiveBox.underrunCount;
        if (_receiveBox.underrunCount % 100 == 0)
        {
            logger::info("%u underrun %u, samples %zu, TD %ums",
                "AudioReceivePipeline",
                _ssrc,
                _receiveBox.underrunCount,
                bufferLevel,
                _targetDelay * 1000 / _rtpFrequency);
        }

        if (bufferLevel > 0)
        {
            _pcmData.fetch(_receiveBox.audio, sampleCount * CHANNELS);
            codec::AudioLinearFade fader(bufferLevel);
            fader.fadeOutStereo(_receiveBox.audio, bufferLevel);
            logger::debug("fade out, TD %ums", "AudioReceivePipeline", _targetDelay * 1000 / _rtpFrequency);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        else if (_receiveBox.underrunCount == 1)
        {
            _pcmData.replay(_receiveBox.audio, sampleCount * CHANNELS);
            codec::swingTail(_receiveBox.audio, 48000, sampleCount);
            logger::debug("%u appended tail, TD %ums",
                "AudioReceivePipeline",
                _ssrc,
                _targetDelay * 1000 / _rtpFrequency);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        return 0;
    }

    _pcmData.fetch(_receiveBox.audio, sampleCount * CHANNELS);
    if (_receiveBox.underrunCount > 0)
    {
        logger::debug("%u fade in after %u underruns, TD %u",
            "AudioReceivePipeline",
            _ssrc,
            _receiveBox.underrunCount,
            _targetDelay);
        codec::AudioLinearFade fader(sampleCount);
        fader.fadeInStereo(_receiveBox.audio, sampleCount);
        _receiveBox.underrunCount = 0;
        _receiveBox.audioSampleCount = sampleCount;
    }
    return sampleCount;
}

uint32_t AudioReceivePipeline::totalBufferSize() const
{
    const auto bufferLevel = _pcmData.size() / CHANNELS;
    return bufferLevel +
        (_jitterBuffer.empty() ? 0
                               : _jitterBuffer.getRtpDelay(_head.rtpTimestamp) + _metrics.receivedRtpCyclesPerPacket);
}

void AudioReceivePipeline::process(uint64_t timestamp, bool isSsrcUsed)
{
    while (!_jitterBuffer.empty())
    {
        const auto bufferLevel = _pcmData.size() / CHANNELS;
        if (_jitterEmergency.counter > 0 || bufferLevel >= _samplesPerPacket)
        {
            return;
        }

        const auto header = _jitterBuffer.getFrontRtp();
        const int16_t sequenceAdvance =
            header->sequenceNumber.get() - static_cast<uint16_t>(_head.extendedSequenceNumber & 0xFFFFu);
        const int32_t timestampAdvance = header->timestamp - _head.rtpTimestamp;

        if (sequenceAdvance == 1 && timestampAdvance > static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket) &&
            totalBufferSize() > _targetDelay && timestampAdvance <= static_cast<int32_t>(3 * _samplesPerPacket))
        {
            const auto maxReduction =
                static_cast<int32_t>(totalBufferSize() - math::roundUpMultiple(_targetDelay, _samplesPerPacket));

            if (maxReduction < static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket))
            {
                _pcmData.appendSilence(_metrics.receivedRtpCyclesPerPacket);
                _head.rtpTimestamp += _metrics.receivedRtpCyclesPerPacket;
                continue;
            }
        }

        const uint32_t extendedSequenceNumber = _head.extendedSequenceNumber + sequenceAdvance;
        auto packet = _jitterBuffer.pop();

        int16_t audioData[_samplesPerPacket * 4 * CHANNELS];
        size_t decodedSamples = 0;
        if (isSsrcUsed)
        {
            decodedSamples = decodePacket(extendedSequenceNumber, timestamp, *packet, audioData);
            if (decodedSamples > 0 && sequenceAdvance == 1)
            {
                _metrics.receivedRtpCyclesPerPacket = decodedSamples;
            }
        }
        else
        {
            codec::clearStereo(audioData, _samplesPerPacket);
            decodedSamples = _samplesPerPacket;
        }
        const auto remainingSamples = compact(*packet, audioData, decodedSamples);
        _head.extendedSequenceNumber += sequenceAdvance;
        _head.rtpTimestamp = header->timestamp;

        const uint32_t newBufferLevel = _pcmData.size() / CHANNELS;

        logger::debug("ssrc %u added pcm %u, JB %u, TD %u, eliminated %zu",
            "AudioReceivePipeline",
            _ssrc,
            newBufferLevel,
            totalBufferSize(),
            _targetDelay,
            decodedSamples - remainingSamples);

        if ((header->sequenceNumber.get() % 100) == 0 && !_jitterBuffer.empty())
        {
            logger::debug("%u JB %u", "AudioReceivePipeline", _ssrc, totalBufferSize());
        }
    }
}

// flushes buffers.
// useful if last mixed participant leaves and audio pipelines are not used atm
void AudioReceivePipeline::flush()
{
    _pcmData.clear();
    while (_jitterBuffer.pop()) {}
}

} // namespace codec
