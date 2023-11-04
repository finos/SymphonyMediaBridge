#include "codec/AudioReceivePipeline.h"
#include "codec/AudioFader.h"
#include "codec/AudioLevel.h"
#include "codec/AudioTools.h"
#include "codec/G711.h"
#include "math/helpers.h"
#include "memory/Allocator.h"
#include "rtp/RtpHeader.h"

#define DEBUG_JB 0

#if DEBUG_JB
#define JBLOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define JBLOG(fmt, ...)
#endif
namespace codec
{

namespace
{
bool isDiscardedPacket(const memory::Packet& packet)
{
    const auto* header = rtp::RtpHeader::fromPacket(packet);
    // check if payload length is zero
    return 0 == (packet.getLength() - header->headerLength());
}
} // namespace

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
      _bufferAtTwoFrames(0),
      _pcmData(7 * _config.channels * _samplesPerPacket, _config.channels * _samplesPerPacket),
      _receiveBox(memory::page::alignedSpace(sizeof(int16_t) * _config.channels * _samplesPerPacket))
{
}

bool AudioReceivePipeline::updateTargetDelay(double delay)
{
    const auto decodeTime = 3;
    const auto prevDelay = _targetDelay;

    _targetDelay = (_estimator.getJitterMaxStable() + 1 + decodeTime) * _rtpFrequency / 1000;

    if (_targetDelay > prevDelay + _rtpFrequency / 100)
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
                audioData += _config.channels * decodedFrames;
            }
        }

        const auto opusPayloadLength = packet.getLength() - header->headerLength();
        const auto decodedFrames =
            _decoder.conceal(header->getPayload(), opusPayloadLength, reinterpret_cast<uint8_t*>(audioData));
        if (decodedFrames > 0)
        {
            audioData += _config.channels * decodedFrames;
        }
    }

    const auto decodedFrames = _decoder.decode(extendedSequenceNumber,
        header->getPayload(),
        packet.getLength() - header->headerLength(),
        reinterpret_cast<uint8_t*>(audioData),
        _samplesPerPacket);

    if (decodedFrames > 0)
    {
        audioData += _config.channels * decodedFrames;
    }

    const size_t samplesProduced = (audioData - originalAudioStart) / _config.channels;
    if (samplesProduced < _samplesPerPacket / 2)
    {
        logger::warn("%u failed to decode opus %zu", "AudioReceivePipeline", _ssrc, samplesProduced);
        return samplesProduced;
    }

    return samplesProduced;
}

// after a number of packets that could not be compressed using current threshold
// increase the threshold so more samples may be eligible for compression.
void AudioReceivePipeline::adjustReductionPower(uint32_t recentReduction)
{
    if (recentReduction > _config.reduction.expectedCompression)
    {
        _elimination.incompressableCount = 0;
        // keep threshold
        return;
    }
    else
    {
        ++_elimination.incompressableCount;
        if (_elimination.incompressableCount > _config.reduction.failedCompressLimit)
        {
            _elimination.deltaThreshold *= _config.reduction.incrementFactor;
        }
    }
}

size_t AudioReceivePipeline::reduce(const memory::Packet& packet,
    int16_t* audioData,
    const uint32_t samples,
    const uint32_t totalJitterSize)
{
    int audioLevel = 0;
    if (!rtp::getAudioLevel(packet, _audioLevelExtensionId, audioLevel))
    {
        audioLevel = codec::computeAudioLevel(audioData, samples * _config.channels);
    }
    _noiseFloor.update(audioLevel);

    auto allowedReduction =
        static_cast<int32_t>(totalJitterSize - math::roundUpMultiple(_targetDelay, _samplesPerPacket));
    if (allowedReduction == 0 && _bufferAtTwoFrames > _config.safeZoneCountBeforeReducingJB &&
        _targetDelay <= _samplesPerPacket * 3 / 4)
    {
        // target delay < 1 frame and buffer has been at two frames for a while
        // start reduction to 1 frame
        allowedReduction = _samplesPerPacket / 4;
    }

    if (allowedReduction <= 0)
    {
        if (_metrics.eliminatedSamples > 0)
        {
            logger::debug("%u shrunk %u packets, eliminated samples %u, eliminated %u packets, pcm %zu, JB %u, TD %u, "
                          "slowJitter %.2f",
                "AudioReceivePipeline",
                _ssrc,
                _metrics.shrunkPackets,
                _metrics.eliminatedSamples,
                _metrics.eliminatedPackets,
                _pcmData.size() / _config.channels,
                jitterBufferSize(_head.nextRtpTimestamp),
                _targetDelay,
                _estimator.getJitterMaxStable());
            _metrics.shrunkPackets = 0;
            _metrics.eliminatedSamples = 0;
            _metrics.eliminatedPackets = 0;
            _elimination.deltaThreshold = _config.reduction.sampleThreshold;
        }
        return samples;
    }
    else
    {
        JBLOG("%u bufsize %u, red %u", "AudioReceivePipeline", _ssrc, totalJitterSize, allowedReduction);
        if (audioLevel > _noiseFloor.getLevel() - _config.reduction.silenceMargin &&
            allowedReduction >= static_cast<int32_t>(_samplesPerPacket))
        {
            ++_metrics.eliminatedPackets;
            _metrics.eliminatedSamples += samples;
            return 0;
        }
        else
        {
            const auto newSampleCount = codec::compactStereoTroughs(audioData,
                samples,
                allowedReduction,
                _config.reduction.silenceZone,
                _elimination.deltaThreshold);
            adjustReductionPower(samples - newSampleCount);

            if (newSampleCount < samples)
            {
                ++_metrics.shrunkPackets;
            }

            _metrics.eliminatedSamples += samples - newSampleCount;
            return newSampleCount;
        }
    }
}

void AudioReceivePipeline::init(uint32_t extendedSequenceNumber, const rtp::RtpHeader& header, uint64_t receiveTime)
{
    _estimator.update(receiveTime, header.timestamp);
    _ssrc = header.ssrc.get();
    _targetDelay = 25 * _rtpFrequency / 1000;
    _head.nextRtpTimestamp = header.timestamp;
    _head.extendedSequenceNumber = header.sequenceNumber - 1;
    _pcmData.appendSilence(_samplesPerPacket);
}

double AudioReceivePipeline::analysePacketJitter(uint32_t extendedSequenceNumber,
    const rtp::RtpHeader& header,
    uint64_t receiveTime)
{
    const double delay = _estimator.update(receiveTime, header.timestamp);
    const auto delayInRtpCycles = delay * _rtpFrequency / 1000;

    if (_targetDelay < delayInRtpCycles && delayInRtpCycles > _metrics.receivedRtpCyclesPerPacket && _pcmData.empty())
    {
        if (!_jitterEmergency.counter)
        {
            _jitterEmergency.sequenceStart = extendedSequenceNumber;
        }
        ++_jitterEmergency.counter;
        logger::debug("%u received late packet %u with delay of %0.2fms and pcm buffer is empty. "
                      "JB %u Replay paused to avoid stuttering",
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
    const auto header = rtp::RtpHeader::fromPacket(*packet);
    if (!header)
    {
        assert(false);
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
        if (_targetDelay < _samplesPerPacket && _pcmData.size() >= _samplesPerPacket)
        {
            ++_bufferAtTwoFrames;
        }
        else
        {
            _bufferAtTwoFrames = 0;
        }
    }
    else if (_jitterEmergency.counter > 0)
    {
        flush(); // reset and start over
        logger::warn("%u RTP delay irrecoverable. Jitter buffer is full. Resetting...", "AudioReceivePipeline", _ssrc);
        return false;
    }

    process(receiveTime);

    return posted;
}

// RTP was received but discarded in transport
// We must track extended sequence number
bool AudioReceivePipeline::onSilencedRtpPacket(uint32_t extendedSequenceNumber,
    memory::UniquePacket packet,
    uint64_t receiveTime)
{
    assert(isDiscardedPacket(*packet));
    return onRtpPacket(extendedSequenceNumber, std::move(packet), receiveTime);
}

// Fetch audio and suppress pops after underruns as well as resume
size_t AudioReceivePipeline::fetchStereo(size_t sampleCount)
{
    codec::clearStereo(_receiveBox.audio, _samplesPerPacket);
    _receiveBox.audioSampleCount = 0;
    const uint32_t bufferLevel = _pcmData.size() / _config.channels;
    if (bufferLevel < sampleCount)
    {
        ++_receiveBox.underrunCount;
        if (_receiveBox.underrunCount % 100 == 1)
        {
            logger::info("%u underrun %u, samples %u",
                "AudioReceivePipeline",
                _ssrc,
                _receiveBox.underrunCount,
                bufferLevel);
        }

        if (bufferLevel > 0)
        {
            _pcmData.fetch(_receiveBox.audio, sampleCount * _config.channels);
            codec::AudioLinearFade fader(bufferLevel);
            fader.fadeOutStereo(_receiveBox.audio, bufferLevel);
            logger::debug("%u fade out %u, requested %zu, pcm %zu",
                "AudioReceivePipeline",
                _ssrc,
                bufferLevel,
                sampleCount,
                _pcmData.size() / _config.channels);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        else if (_receiveBox.underrunCount == 1)
        {
            _pcmData.replay(_receiveBox.audio, sampleCount * _config.channels);
            codec::swingTail(_receiveBox.audio, 48000, sampleCount);
            logger::debug("%u appended tail", "AudioReceivePipeline", _ssrc);
            _receiveBox.audioSampleCount = sampleCount;
            return sampleCount;
        }
        return 0;
    }

    // JBLOG("fetched %zu", "AudioReceivePipeline", sampleCount);
    _pcmData.fetch(_receiveBox.audio, sampleCount * _config.channels);
    _receiveBox.audioSampleCount = sampleCount;

    if (_receiveBox.underrunCount > 0)
    {
        logger::debug("%u fade in after %u underruns", "AudioReceivePipeline", _ssrc, _receiveBox.underrunCount);
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

// If it is less than 10ms since data was still not fetched from pcm buffer
// we can wait another cycle before assuming packet is lost rather than re-ordered.
bool AudioReceivePipeline::shouldWaitForMissingPacket(uint64_t timestamp) const
{
    const int32_t halfPtime = _samplesPerPacket * utils::Time::sec / (2 * _rtpFrequency);
    return static_cast<int32_t>(timestamp - _head.readyPcmTimestamp) <= halfPtime;
}

// insert DTX silence if the gap is reasonably small to keep speak pace.
// If we are lagging behind we use it to catch up instead
bool AudioReceivePipeline::dtxHandler(const int16_t sequenceAdvance,
    const int64_t timestampAdvance,
    const uint32_t totalJitterBufferSize)
{
    const bool isDTX =
        sequenceAdvance == 1 && timestampAdvance > static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket);

    if (isDTX && timestampAdvance <= static_cast<int32_t>(3 * _samplesPerPacket))
    {
        const auto allowedReduction =
            static_cast<int32_t>(totalJitterBufferSize - math::roundUpMultiple(_targetDelay, _samplesPerPacket));

        if (allowedReduction < static_cast<int32_t>(_metrics.receivedRtpCyclesPerPacket))
        {
            JBLOG("%u DTX silence", "AudioReceivePipeline", _ssrc);
            _pcmData.appendSilence(_metrics.receivedRtpCyclesPerPacket - std::max(allowedReduction, 0));
            _head.nextRtpTimestamp += _metrics.receivedRtpCyclesPerPacket;
        }
        return true;
    }
    return false;
}

namespace
{
bool isG711(const memory::Packet& packet)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
    return rtpHeader->payloadType == codec::Pcma::payloadType || rtpHeader->payloadType == codec::Pcmu::payloadType;
}
} // namespace

void AudioReceivePipeline::process(const uint64_t timestamp)
{
    size_t bufferLevel = _pcmData.size() / _config.channels;

    for (; _jitterEmergency.counter == 0 && !_jitterBuffer.empty() && bufferLevel < _samplesPerPacket;
         bufferLevel = _pcmData.size() / _config.channels)
    {
        const auto header = _jitterBuffer.getFrontRtp();
        const int16_t sequenceAdvance =
            header->sequenceNumber.get() - static_cast<uint16_t>(_head.extendedSequenceNumber & 0xFFFFu);

        // Re-ordering.
        // previous packet already played
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

        // Re-ordering, or loss
        if (sequenceAdvance > 1 && shouldWaitForMissingPacket(timestamp))
        {
            return;
        }

        const int32_t timestampAdvance = header->timestamp - _head.nextRtpTimestamp;
        const uint32_t totalJitterBufferSize = jitterBufferSize(_head.nextRtpTimestamp) + bufferLevel;

        if (dtxHandler(sequenceAdvance, timestampAdvance, totalJitterBufferSize))
        {
            continue;
        }

        // Decode, reduce and append the packet
        const uint32_t extendedSequenceNumber = _head.extendedSequenceNumber + sequenceAdvance;
        auto packet = _jitterBuffer.pop();

        int16_t audioData[_samplesPerPacket * 4 * _config.channels];
        size_t decodedSamples = 0;

        if (isDiscardedPacket(*packet))
        {
            if (!isG711(*packet))
            {
                decodedSamples = std::max(480u, _metrics.receivedRtpCyclesPerPacket);
                _decoder.onUnusedPacketReceived(extendedSequenceNumber);
            }
            std::memset(audioData, 0, decodedSamples);
        }
        else
        {
            if (!isG711(*packet))
            {
                decodedSamples = decodePacket(extendedSequenceNumber, timestamp, *packet, audioData);
            }
            else
            {
                decodedSamples = decodeG711(extendedSequenceNumber, timestamp, *packet, audioData);
            }
            if (decodedSamples > 0 && sequenceAdvance == 1)
            {
                _metrics.receivedRtpCyclesPerPacket = decodedSamples;
            }
        }

        _head.extendedSequenceNumber = extendedSequenceNumber;
        _head.nextRtpTimestamp = header->timestamp + _metrics.receivedRtpCyclesPerPacket;
        const auto remainingSamples = reduce(*packet, audioData, decodedSamples, totalJitterBufferSize);
        if (_pcmData.append(audioData, remainingSamples * _config.channels))
        {
            JBLOG("ssrc %u added pcm %zu, JB %u (%u), TD %u, eliminated %zu, tstmp %u",
                "AudioReceivePipeline",
                _ssrc,
                _pcmData.size() / _config.channels,
                jitterBufferSize(_head.nextRtpTimestamp),
                _jitterBuffer.count(),
                _targetDelay,
                decodedSamples - remainingSamples,
                _head.nextRtpTimestamp);
        }
        else
        {
            const auto header = rtp::RtpHeader::fromPacket(*packet);
            logger::warn("%u failed to append seq %u ts %u",
                "AudioReceivePipeline",
                _ssrc,
                header->sequenceNumber.get(),
                header->timestamp.get());
        }

        if ((header->sequenceNumber.get() % 100) == 0 && !_jitterBuffer.empty())
        {
            JBLOG("%u pcm %zu JB %u",
                "AudioReceivePipeline",
                _ssrc,
                _pcmData.size() / _config.channels,
                jitterBufferSize(_head.nextRtpTimestamp));
        }
    }

    if (bufferLevel >= _samplesPerPacket)
    {
        // track when we last had enough data in buffer so we know how long we may wait for more
        _head.readyPcmTimestamp = timestamp;
    }
}

// flushes buffers.
// useful if last mixed participant leaves and audio pipelines are not used atm
void AudioReceivePipeline::flush()
{
    _pcmData.clear();
    while (_jitterBuffer.pop())
    {
    }
    _targetDelay = 0; // will cause start over on seqno, rtp timestamp and jitter assessment
    _jitterEmergency.counter = 0;
    _bufferAtTwoFrames = 0;
    _elimination = SampleElimination();
}

size_t AudioReceivePipeline::decodeG711(uint32_t extendedSequenceNumber,
    const uint64_t timestamp,
    const memory::Packet& packet,
    int16_t* audioData)
{
    const auto header = rtp::RtpHeader::fromPacket(packet);
    const int16_t* originalAudioStart = audioData;
    const size_t sampleCount = packet.getLength() - header->headerLength();

    if (header->payloadType == codec::Pcma::payloadType)
    {
        _pcmaDecoder.decode(header->getPayload(), audioData, sampleCount);
        codec::makeStereo(audioData, sampleCount * 6);
        return sampleCount * 6;
    }

    if (header->payloadType == codec::Pcmu::payloadType)
    {
        _pcmuDecoder.decode(header->getPayload(), audioData, sampleCount);
        codec::makeStereo(audioData, sampleCount * 6);
        return sampleCount * 6;
    }
}

} // namespace codec
