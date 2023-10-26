#pragma once
#include "codec/NoiseFloor.h"
#include "codec/OpusDecoder.h"
#include "codec/SpscAudioBuffer.h"
#include "rtp/JitterBufferList.h"
#include "rtp/JitterEstimator.h"
#include "utils/Time.h"
#include <atomic>

namespace codec
{

/**
 * Audio receive pipe line that performs adaptive jitter buffering and cut off concealment.
 * PCM data buffer is single produce single consumer thread safe. The rest shall run on a single thread context.
 *
 * Mechanisms:
 * - If buffers run empty, the tail of the audio is faded out to avoid pops and clicks.
 * - When sound continues after an underrun, the audio is faded in to avoid pops.
 * - Audio elements with significant sound is not dropped. Silence is removed and audio is
 * time compressed with some distortion where it is heard the least. This avoids CPU intensive
 * resampling and filtering.
 * - Packet loss is concealed by Opus decoder.
 * - If jitter is low, the pipe line will operate in the time window between packet arrival and audio fetch.
 * Which can be less than one audio frame.
 *
 * Packet arrival may be offset from packet pull pace. If jitter is less than the time left to pull,
 * the packets will arrive before pull and can be put into pcm buffer immediately. Otherwise, the pipe line will
 * have one packet in pcm and 1 or 0 packets in JB.
 *
 *       |    |   |   |
 *       v    v   v   v
 *         |    |    |    |
 *         v    v    v    v
 */
class AudioReceivePipeline
{
    struct Config
    {
        uint32_t channels = 2;
        uint32_t safeZoneCountBeforeReducingJB = 150;

        struct
        {
            uint32_t sampleThreshold = 10;
            uint32_t expectedCompression = 30;
            uint32_t failedCompressLimit = 15;
            double incrementFactor = 1.25;
            double silenceMargin = 6.0; // dB
            double silenceZone = 10.0;
        } reduction;
    };
    const Config _config;

public:
    AudioReceivePipeline(uint32_t rtpFrequency, uint32_t ptime, uint32_t maxPackets, int audioLevelExtensionId = 255);

    // called from same thread context
    bool onRtpPacket(uint32_t extendedSequenceNumber, memory::UniquePacket packet, uint64_t receiveTime);
    bool onSilencedRtpPacket(uint32_t extendedSequenceNumber, memory::UniquePacket packet, uint64_t receiveTime);

    void process(uint64_t timestamp);
    void flush();

    // called from mix consumer
    bool needProcess() const { return _pcmData.size() < _samplesPerPacket * _config.channels; }
    size_t fetchStereo(size_t sampleCount);

    const int16_t* getAudio() const { return _receiveBox.audio; }
    uint32_t getAudioSampleCount() const { return _receiveBox.audioSampleCount; }

private:
    void init(uint32_t extendedSequenceNumber, const rtp::RtpHeader& header, uint64_t receiveTime);
    double analysePacketJitter(uint32_t extendedSequenceNumber, const rtp::RtpHeader& header, uint64_t receiveTime);
    bool updateTargetDelay(double delayMs);
    size_t decodePacket(uint32_t extendedSequenceNumber,
        uint64_t timestamp,
        const memory::Packet& packet,
        int16_t* audioData);
    size_t reduce(const memory::Packet& packet, int16_t* audioData, uint32_t samples, uint32_t totalJitterSize);
    uint32_t jitterBufferSize(uint32_t rtpTimestamp) const;
    void adjustReductionPower(uint32_t recentReduction);

    bool shouldWaitForMissingPacket(uint64_t timestamp) const;
    bool dtxHandler(int16_t seqAdvance, int64_t timestampAdvance,uint32_t totalJitterBufferSize);

    uint32_t _ssrc;
    const uint32_t _rtpFrequency;
    const uint32_t _samplesPerPacket;

    rtp::JitterBufferList _jitterBuffer;
    rtp::JitterEstimator _estimator;

    const int _audioLevelExtensionId;
    codec::OpusDecoder _decoder;
    codec::NoiseFloor _noiseFloor;

    uint32_t _targetDelay;
    struct SampleElimination
    {
        uint32_t incompressableCount = 0;
        int16_t deltaThreshold = 10;
    } _elimination;

    struct HeadInfo
    {
        uint64_t readyPcmTimestamp = 0; // last time we saw pcm data pending in
        uint32_t nextRtpTimestamp = 0;
        uint32_t extendedSequenceNumber = 0;
    } _head;

    struct JitterEmergency
    {
        uint32_t counter = 0; // late packet arrives and buffer is empty
        uint32_t sequenceStart = 0;
    } _jitterEmergency;

    // Count how many times buffer has been at 2 frames. If target delay is low we can reduce to one frame
    uint32_t _bufferAtTwoFrames;

    struct Metrics
    {
        uint32_t shrunkPackets = 0;
        uint32_t eliminatedPackets = 0;
        uint32_t eliminatedSamples = 0;
        uint32_t receivedRtpCyclesPerPacket = 960; // 480, 960, 1440
    } _metrics;

    SpscAudioBuffer<int16_t> _pcmData;

    // for receive thread
    struct ReceiveBox
    {
        ReceiveBox(size_t bufferSize);
        ~ReceiveBox();

        uint32_t underrunCount;
        const size_t audioBufferSize;
        int16_t* audio;
        uint32_t audioSampleCount;
    } _receiveBox;
};

} // namespace codec
