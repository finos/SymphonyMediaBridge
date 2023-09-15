#pragma once
#include "codec/NoiseFloor.h"
#include "codec/OpusDecoder.h"
#include "rtp/JitterBuffer.h"
#include "rtp/JitterEstimator.h"
#include "rtp/SpscAudioBuffer.h"
#include "utils/Time.h"
#include <atomic>

namespace rtp
{

/**
 * Audio receive pipe line that performs adaptive jitter buffering and cut off concealment.
 * PCM data buffer is single produce single consumer thread safe. The rest shall run on a single thread context.
 */
class AudioReceivePipeline
{
public:
    AudioReceivePipeline(uint32_t rtpFrequency, uint32_t ptime, uint32_t maxPackets, int audioLevelExtensionId = 255);
    ~AudioReceivePipeline();

    // called from same thread context
    bool onRtpPacket(uint32_t extendedSequenceNumber, memory::UniquePacket packet, uint64_t receiveTime);

    void process(uint64_t timestamp);

    uint32_t getTargetDelay() const { return _targetDelay; }
    uint32_t size() const { return _buffer.count(); }

    bool needProcess() const { return _pcmData.size() <= _samplesPerPacket * 2; }
    size_t fetchStereo(size_t sampleCount);

    const int16_t* getAudio() const { return _audio; }

private:
    bool updateTargetDelay(double delayMs);
    size_t decodePacket(uint32_t extendedSequenceNumber,
        uint64_t timestamp,
        const memory::Packet& packet,
        int16_t* audioData);
    size_t compact(const memory::Packet& packet, int16_t* audioData, size_t samples);
    void replayFadeOut(int16_t* buffer);

    const uint32_t _rtpFrequency;
    const uint32_t _samplesPerPacket;

    JitterBuffer _buffer;
    JitterEstimator _estimator;

    const int _audioLevelExtensionId;
    codec::OpusDecoder _decoder;
    codec::NoiseFloor _noiseFloor;

    uint32_t _targetDelay;

    struct HeadInfo
    {
        uint32_t timestamp = 0;
        uint32_t rtpTimestamp = 0;
        uint32_t extendedSequenceNumber = 0;
    } _head;

    struct JitterEmergency
    {
        int counter = 0; // late packet arrives and buffer is empty
        uint32_t sequenceStart = 0;
    } _jitterEmergency;

    struct Metrics
    {
        uint32_t shrunkPackets = 0;
        uint32_t eliminatedPackets = 0;
        uint32_t eliminatedSamples = 0;
    } _metrics;

    SpscAudioBuffer<int16_t> _pcmData;

    // for receive thread
    uint32_t _underrunCount;
    const size_t _audioBufferSize;
    int16_t* _audio;
};

} // namespace rtp
