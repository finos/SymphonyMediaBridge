#include "rtp/AudioReceivePipeline.h"
#include "codec/AudioFader.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"

namespace rtp
{

AudioReceivePipeline::AudioReceivePipeline(uint32_t rtpFrequency,
    uint32_t ptime,
    uint32_t maxPackets,
    int audioLevelExtensionId)
    : _rtpFrequency(rtpFrequency),
      _samplesPerPacket(ptime * rtpFrequency / 1000),
      _pcmData(7 * 2 * _samplesPerPacket, 2 * rtpFrequency / 50),
      _buffer(maxPackets),
      _estimator(rtpFrequency),
      _audioLevelExtensionId(audioLevelExtensionId),
      _targetDelay(0),
      _underrunCount(0)
{
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

void AudioReceivePipeline::decodePacket(uint32_t extendedSequenceNumber,
    uint64_t timestamp,
    const memory::Packet& packet)
{
    int16_t audioData[_samplesPerPacket * 2];
    const auto header = RtpHeader::fromPacket(packet);

    if (_decoder.hasDecoded() && extendedSequenceNumber != _decoder.getExpectedSequenceNumber())
    {
        const int32_t lossCount = static_cast<int32_t>(extendedSequenceNumber - _decoder.getExpectedSequenceNumber());
        if (lossCount <= 0)
        {
            logger::debug("Old opus packet sequence %u expected %u, discarding",
                "AudioReceivePipeline",
                extendedSequenceNumber,
                _decoder.getExpectedSequenceNumber());
            return;
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
                if (!_pcmData.append(audioData, decodedFrames * 2))
                {
                    logger::warn("failed to add concealment data seq %u",
                        "AudioReceivePipeline",
                        header->sequenceNumber.get());
                }
                _head.rtpTimestamp += _samplesPerPacket;
                _head.timestamp += utils::Time::ms * 20;
            }
        }

        const auto opusPayloadLength = packet.getLength() - header->headerLength();
        const auto decodedFrames =
            _decoder.conceal(header->getPayload(), opusPayloadLength, reinterpret_cast<uint8_t*>(audioData));
        if (decodedFrames > 0)
        {
            if (!_pcmData.append(audioData, decodedFrames * 2))
            {
                logger::warn("failed to add concealment data seq %u",
                    "AudioReceivePipeline",
                    header->sequenceNumber.get());
            }
            _head.rtpTimestamp += _samplesPerPacket;
            _head.timestamp += utils::Time::ms * 20;
        }
    }

    auto decodedSamples = _decoder.decode(extendedSequenceNumber,
        header->getPayload(),
        packet.getLength() - header->headerLength(),
        reinterpret_cast<uint8_t*>(audioData),
        _samplesPerPacket);

    if (decodedSamples < static_cast<int32_t>(_samplesPerPacket))
    {
        logger::warn("failed to decode opus %d", "AudioReceivePipeline", decodedSamples);
        return;
    }

    int lvl = 0;
    if (!rtp::getAudioLevel(packet, _audioLevelExtensionId, lvl))
    {
        lvl = codec::computeAudioLevel(audioData, _samplesPerPacket * 2);
    }
    _noiseFloor.update(lvl);

    bool mustCompact = !_buffer.empty() && !_pcmData.empty() &&
        (_buffer.getTailRtp()->timestamp - _head.rtpTimestamp + _pcmData.size() / 2 > _targetDelay + 20);

    if (mustCompact)
    {
        if (lvl > _noiseFloor.getLevel() - 6 && _pcmData.size() > _samplesPerPacket)
        {
            ++_metrics.eliminatedPackets;
        }
        else
        {
            ++_metrics.shrinks;
            int newSampleCount = codec::compactStereo(audioData, _samplesPerPacket);
            _pcmData.append(audioData, newSampleCount * 2);
        }
    }
    else
    {
        if (_metrics.shrinks > 0 || _metrics.eliminatedPackets > 0)
        {
            logger::debug("shrunk %u packets, eliminated %u packets",
                "AudioReceivePipeline",
                _metrics.shrinks,
                _metrics.eliminatedPackets);
            _metrics.shrinks = 0;
            _metrics.eliminatedPackets = 0;
        }
        auto result = _pcmData.append(audioData, _samplesPerPacket * 2);
        if (!result)
        {
            logger::warn("failed to append seq %u ts %u", "", header->sequenceNumber.get(), header->timestamp.get());
        }
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
size_t AudioReceivePipeline::fetchStereo(int16_t* buffer, size_t sampleCount)
{
    const auto currentSize = _pcmData.size();
    if (currentSize < sampleCount * 2)
    {
        ++_underrunCount;
        logger::info("underrun %u", "AudioReceivePipeline", _underrunCount);
        if (currentSize > 0)
        {
            _pcmData.fetch(buffer, sampleCount * 2);
            codec::AudioLinearFade fader(currentSize / 2);
            codec::fadeOutStereo(buffer, currentSize / 2, fader);
            logger::debug("fade out", "AudioReceivePipeline");
            _pcmData.popFront(sampleCount * 2);
            return sampleCount;
        }
        else if (_underrunCount == 1)
        {
            _pcmData.replay(buffer, sampleCount * 2);

            codec::sineTail(buffer, 250, 48000, sampleCount);
            codec::AudioLinearFade fader(sampleCount);
            codec::fadeOutStereo(buffer, sampleCount, fader);
            logger::debug("appended sine tail", "AudioReceivePipeline");
            return sampleCount;
        }
        return 0;
    }

    _pcmData.fetch(buffer, sampleCount * 2);
    _pcmData.popFront(sampleCount * 2);
    if (_underrunCount > 0)
    {
        logger::debug("fade in", "AudioReceivePipeline");
        codec::AudioLinearFade fader(sampleCount);
        codec::fadeInStereo(buffer, sampleCount, fader);
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
        const bool isDue = (header->timestamp - _head.rtpTimestamp + _pcmData.size() / 2) <=
            (timestamp - _head.timestamp) * _rtpFrequency / utils::Time::sec;

        const auto bufferLevel = _pcmData.size();
        if (_jitterEmergency.counter > 0) {}
        else if ((bufferLevel <= 2 * _samplesPerPacket && isDue) ||
            (bufferLevel > 0 && _pcmData.size() <= 2 * _samplesPerPacket && isNext))
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

            decodePacket(extendedSequenceNumber, timestamp, *packet);
            _head.rtpTimestamp = header->timestamp + _samplesPerPacket;
            _head.timestamp += utils::Time::ms * 20;

            if ((header->sequenceNumber.get() % 100) == 0 && !_buffer.empty())
            {
                auto tail = _buffer.getTailRtp();
                logger::debug("ssrc %u, lag %ums",
                    "AudioReceivePipeline",
                    header->ssrc.get(),
                    static_cast<uint32_t>(
                        tail->timestamp.get() - _head.rtpTimestamp + _samplesPerPacket + _pcmData.size() / 2) *
                        1000 / _rtpFrequency);
            }
        }
    }
}

} // namespace rtp
