#include "codec/AudioReceivePipeline.h"
#include "codec/AudioFader.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "math/helpers.h"
#include "memory/Allocator.h"

#define DEBUG_JB 1

#if DEBUG_JB
#define JBLOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define JBLOG(fmt, ...)
#endif
namespace codec
{
const size_t CHANNELS = 2;
const uint32_t REDUCTION_THRESHOLD = 10;

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

void AudioReceivePipeline::adjustReductionPower(uint32_t recentReduction)
{
    if (recentReduction > 30)
    {
        _elimination.uncompressableCount = 0;
        // keep threshold
        return;
    }
    else
    {
        ++_elimination.uncompressableCount;
        if (_elimination.uncompressableCount > 15)
        {
            _elimination.deltaThreshold *= 1.25;
        }
    }
}

size_t AudioReceivePipeline::reduce(const memory::Packet& packet,
    int16_t* audioData,
    uint32_t samples,
    uint32_t totalJitterSize)
{
    int audioLevel = 0;
    if (!rtp::getAudioLevel(packet, _audioLevelExtensionId, audioLevel))
    {
        audioLevel = codec::computeAudioLevel(audioData, samples * CHANNELS);
    }
    _noiseFloor.update(audioLevel);

    const auto bufferLevel = _pcmData.size() / CHANNELS;
    const uint32_t bufferSize =
        jitterBufferSize(_head.nextRtpTimestamp) + bufferLevel + _metrics.receivedRtpCyclesPerPacket;
    const auto maxReduction = static_cast<int32_t>(bufferSize - math::roundUpMultiple(_targetDelay, _samplesPerPacket));

    if (maxReduction > static_cast<int32_t>(_rtpFrequency / 200))
    {
        logger::debug("%u bufsize %u, red %u", "AudioReceivePipeline", _ssrc, bufferSize, maxReduction);
        if (audioLevel > _noiseFloor.getLevel() - 6 && maxReduction >= static_cast<int32_t>(_samplesPerPacket))
        {
            ++_metrics.eliminatedPackets;
            _metrics.eliminatedSamples += samples;
            return 0;
        }
        else
        {
            const auto newSampleCount =
                codec::compactStereoTroughs(audioData, samples, maxReduction, 10, _elimination.deltaThreshold);
            adjustReductionPower(samples - newSampleCount);

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
                jitterBufferSize(_head.nextRtpTimestamp),
                _targetDelay,
                _estimator.getJitterMaxStable());
            _metrics.shrunkPackets = 0;
            _metrics.eliminatedSamples = 0;
            _metrics.eliminatedPackets = 0;
            _elimination.deltaThreshold = REDUCTION_THRESHOLD;
        }
        return samples;
    }
}

void AudioReceivePipeline::init(uint32_t extendedSequenceNumber, rtp::RtpHeader& header, uint64_t receiveTime)
{
    _estimator.update(receiveTime, header.timestamp);
    _ssrc = header.ssrc.get();
    _targetDelay = 25 * _rtpFrequency / 1000;
    _head.nextRtpTimestamp = header.timestamp;
    _head.extendedSequenceNumber = header.sequenceNumber - 1;
    _pcmData.appendSilence(_samplesPerPacket);
}

double AudioReceivePipeline::analysePacketJitter(uint32_t extendedSequenceNumber,
    rtp::RtpHeader& header,
    uint64_t receiveTime)
{
    const double delay = _estimator.update(receiveTime, header.timestamp);
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
    else if (_jitterEmergency.counter > 0 &&
        delayInRtpCycles < _jitterBuffer.getRtpDelay() + _metrics.receivedRtpCyclesPerPacket)
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

    return delay;
}

bool AudioReceivePipeline::onRtpPacket(uint32_t extendedSequenceNumber,
    memory::UniquePacket packet,
    uint64_t receiveTime)
{
    auto header = rtp::RtpHeader::fromPacket(*packet);
    if (!header)
    {
        return false; // corrupt
    }

    if (_targetDelay == 0)
    {
        init(extendedSequenceNumber, *header, receiveTime);
        _jitterBuffer.add(std::move(packet));
        return true;
    }

    const auto delay = analysePacketJitter(extendedSequenceNumber, *header, receiveTime);
    const auto posted = _jitterBuffer.add(std::move(packet));
    if (posted)
    {
        updateTargetDelay(delay);
    }

    process(receiveTime);

    return posted;
}

bool AudioReceivePipeline::onSilencedRtpPacket(uint32_t extendedSequenceNumber,
    memory::UniquePacket packet,
    uint64_t receiveTime)
{
    auto header = rtp::RtpHeader::fromPacket(*packet);
    if (!header)
    {
        return false; // corrupt
    }

    if (_targetDelay == 0)
    {
        init(extendedSequenceNumber, *header, receiveTime);
        _jitterBuffer.add(std::move(packet));
        return true;
    }

    const auto delay = analysePacketJitter(extendedSequenceNumber, *header, receiveTime);

    const auto posted = _jitterBuffer.add(std::move(packet));
    if (posted)
    {
        updateTargetDelay(delay);
    }

    process(receiveTime);
    return true;
}

// Fetch audio and suppress pops after underruns as well as resume
size_t AudioReceivePipeline::fetchStereo(size_t sampleCount)
{
    codec::clearStereo(_receiveBox.audio, _samplesPerPacket);
    const uint32_t bufferLevel = _pcmData.size() / CHANNELS;
    if (bufferLevel < sampleCount)
    {
        ++_receiveBox.underrunCount;
        if (_receiveBox.underrunCount % 100 == 0)
        {
            logger::info("%u underrun %u, samples %u, TD %ums",
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
            logger::debug("%u fade out %u, TD %ums, requested %zu, pcm %zu",
                "AudioReceivePipeline",
                _ssrc,
                bufferLevel,
                _targetDelay * 1000 / _rtpFrequency,
                sampleCount,
                _pcmData.size() / CHANNELS);
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

    // JBLOG("fetched %zu", "AudioReceivePipeline", sampleCount);
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

uint32_t AudioReceivePipeline::jitterBufferSize(uint32_t rtpTimestamp) const
{
    return (_jitterBuffer.empty() ? 0 : _jitterBuffer.getRtpDelay(rtpTimestamp) + _metrics.receivedRtpCyclesPerPacket);
}

void AudioReceivePipeline::process(uint64_t timestamp)
{
    size_t bufferLevel = _pcmData.size() / CHANNELS;

    for (; _jitterEmergency.counter == 0 && !_jitterBuffer.empty() && bufferLevel < _samplesPerPacket;
         bufferLevel = _pcmData.size() / CHANNELS)
    {
        const auto header = _jitterBuffer.getFrontRtp();
        const int16_t sequenceAdvance =
            header->sequenceNumber.get() - static_cast<uint16_t>(_head.extendedSequenceNumber & 0xFFFFu);
        const int32_t timestampAdvance = header->timestamp - _head.nextRtpTimestamp;

        const uint32_t totalJitterBufferSize = jitterBufferSize(_head.nextRtpTimestamp) + bufferLevel;

        if (sequenceAdvance < 0)
        {
            _jitterBuffer.pop();
            logger::info("%u drop late packet seq %u, tstmp %u",
                "AudioReceivePipeline",
                _ssrc,
                _head.extendedSequenceNumber + sequenceAdvance,
                header->timestamp.get());
            continue;
        }

        // DTX
        const bool isDTX =
            sequenceAdvance == 1 && timestampAdvance > static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket);
        if (isDTX && totalJitterBufferSize > _targetDelay &&
            timestampAdvance <= static_cast<int32_t>(3 * _samplesPerPacket))
        {
            const auto maxReduction =
                static_cast<int32_t>(totalJitterBufferSize - math::roundUpMultiple(_targetDelay, _samplesPerPacket));

            if (maxReduction < static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket))
            {
                JBLOG("%u DTX silence", "AudioReceivePipeline", _ssrc);
                _pcmData.appendSilence(_metrics.receivedRtpCyclesPerPacket);
                _head.nextRtpTimestamp += _metrics.receivedRtpCyclesPerPacket;
                continue;
            }
        }

        const int32_t halfPtime = _samplesPerPacket * utils::Time::sec / (2 * _rtpFrequency);
        if (sequenceAdvance > 1 && static_cast<int32_t>(timestamp - _head.readyPcmTimestamp) <= halfPtime)
        {
            return; // wait a bit more for disordered packet to arrive.
        }

        const uint32_t extendedSequenceNumber = _head.extendedSequenceNumber + sequenceAdvance;
        auto packet = _jitterBuffer.pop();

        int16_t audioData[_samplesPerPacket * 4 * CHANNELS];
        size_t decodedSamples = 0;
        const size_t payloadLength = packet->getLength() - header->headerLength();
        if (payloadLength == 0)
        {
            decodedSamples = std::max(480u, _metrics.receivedRtpCyclesPerPacket);
            _head.nextRtpTimestamp += decodedSamples;
            _decoder.onUnusedPacketReceived(extendedSequenceNumber);
        }
        else
        {
            decodedSamples = decodePacket(extendedSequenceNumber, timestamp, *packet, audioData);
            if (decodedSamples > 0 && sequenceAdvance == 1)
            {
                _metrics.receivedRtpCyclesPerPacket = decodedSamples;
            }
        }

        _head.extendedSequenceNumber = extendedSequenceNumber;
        _head.nextRtpTimestamp = header->timestamp + _metrics.receivedRtpCyclesPerPacket;
        const auto remainingSamples = reduce(*packet, audioData, decodedSamples, totalJitterBufferSize);

        const uint32_t newBufferLevel = _pcmData.size() / CHANNELS;
        JBLOG("ssrc %u added pcm %u, JB %u (%u), TD %u, eliminated %zu, tstmp %u",
            "AudioReceivePipeline",
            _ssrc,
            newBufferLevel,
            jitterBufferSize(_head.nextRtpTimestamp),
            _jitterBuffer.count(),
            _targetDelay,
            decodedSamples - remainingSamples,
            _head.nextRtpTimestamp);

        if ((header->sequenceNumber.get() % 100) == 0 && !_jitterBuffer.empty())
        {
            logger::debug("%u pcm %u JB %u",
                "AudioReceivePipeline",
                _ssrc,
                newBufferLevel,
                jitterBufferSize(_head.nextRtpTimestamp));
        }
    }

    if (bufferLevel >= _samplesPerPacket)
    {
        _head.readyPcmTimestamp = timestamp;
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
