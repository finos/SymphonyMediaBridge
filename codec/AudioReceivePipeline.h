#pragma once
#include "codec/NoiseFloor.h"
#include "codec/OpusDecoder.h"
#include "codec/SpscAudioBuffer.h"
#include "rtp/JitterBuffer.h"
#include "rtp/JitterEstimator.h"
#include "utils/Time.h"
#include <atomic>

namespace codec
{

/**
 * Audio receive pipe line that performs adaptive jitter buffering and cut off concealment.
 * PCM data buffer is single produce single consumer thread safe. The rest shall run on a single thread context.
 */
class AudioReceivePipeline
{
public:
    AudioReceivePipeline(uint32_t rtpFrequency, uint32_t ptime, uint32_t maxPackets, int audioLevelExtensionId = 255);

    // called from same thread context
    bool onRtpPacket(uint32_t extendedSequenceNumber, memory::UniquePacket packet, uint64_t receiveTime);
    bool onSilencedRtpPacket(uint32_t extendedSequenceNumber, memory::UniquePacket packet, uint64_t receiveTime);

    void process(uint64_t timestamp);
    void flush();

    // called from mix consumer
    bool needProcess() const { return _pcmData.size() < _samplesPerPacket * 2; }
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

    uint32_t _ssrc;
    const uint32_t _rtpFrequency;
    const uint32_t _samplesPerPacket;

    rtp::JitterBuffer _jitterBuffer;
    rtp::JitterEstimator _estimator;

    const int _audioLevelExtensionId;
    codec::OpusDecoder _decoder;
    codec::NoiseFloor _noiseFloor;

    uint32_t _targetDelay;
    struct SampleElimination
    {
        uint32_t uncompressableCount = 0;
        int16_t deltaThreshold = 10;
    } _elimination;

    struct HeadInfo
    {
        uint64_t readyPcmTimestamp = 0;
        uint32_t nextRtpTimestamp = 0;
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
        uint32_t receivedRtpCyclesPerPacket = 960;
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
