#include "codec/AudioLevel.h"
#include "codec/OpusDecoder.h"
#include "codec/OpusEncoder.h"
#include "logger/Logger.h"
#include "utils/Time.h"
#include <cmath>
#include <gtest/gtest.h>

namespace codec
{
int computeAudioLevel(const int16_t* payload, int samples);
}

namespace
{
static const double sampleFrequency = 48000;
static const int samples = sampleFrequency / 50;

} // namespace
struct OpusTest : testing::Test
{
    static constexpr double sampleFrequency = 48000;
    static constexpr int32_t samples = 48000 / 50;

    void SetUp() override
    {
        for (int i = 0; i < samples; ++i)
        {
            _pcmData[i * 2] = sin(2 * PI * i * 400 / sampleFrequency) * amplitude;
            _pcmData[i * 2 + 1] = _pcmData[i * 2];
        }
    }

    static constexpr double PI = 3.14159;
    static constexpr double amplitude = 2000;
    int16_t _pcmData[960 * 2];
    uint8_t _opusData[960];
};

TEST_F(OpusTest, encode)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;

    auto start = utils::Time::getAbsoluteTime();
    int32_t opusBytes;
    opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);

    EXPECT_GT(opusBytes, 100);

    int16_t decodeBuffer[samples * 2];
    uint32_t sequenceNumber = 10;
    const auto framesInPacketBuffer = samples;

    auto startDecode = utils::Time::getAbsoluteTime();
    int32_t samplesProduced = 0;
    samplesProduced = decoder.decode(sequenceNumber,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);

    EXPECT_EQ(samplesProduced, samples);
    auto endDecode = utils::Time::getAbsoluteTime();

    logger::info("enc %" PRIu64 "ms dec %" PRIu64 "ms",
        "",
        (startDecode - start) / utils::Time::ms,
        (endDecode - startDecode) / utils::Time::ms);
    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-27), -dB);

    samplesProduced = decoder.decode(sequenceNumber,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);
    EXPECT_EQ(samplesProduced, samples);

    // auto dB = codec::computeAudioLevel(_pcmData, samples);
    // EXPECT_EQ(static_cast<int>(-31), -dB);
}

TEST_F(OpusTest, healing)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;

    int16_t decodeBuffer[samples * 2];
    uint32_t sequenceNumber = 10;
    const auto framesInPacketBuffer = samples;

    auto opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);
    EXPECT_GT(opusBytes, 100);

    auto samplesProduced = decoder.decode(sequenceNumber,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);
    EXPECT_EQ(samplesProduced, samples);

    samplesProduced = decoder.conceal(reinterpret_cast<unsigned char*>(decodeBuffer));
    EXPECT_EQ(samplesProduced, samples);
    samplesProduced = decoder.conceal(reinterpret_cast<unsigned char*>(decodeBuffer));

    EXPECT_EQ(samplesProduced, samples);
    samplesProduced = decoder.conceal(_opusData, opusBytes, reinterpret_cast<unsigned char*>(decodeBuffer));
    EXPECT_EQ(samplesProduced, samples);

    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-27), -dB);
}

TEST_F(OpusTest, seqSkip)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;
    int16_t decodeBuffer[samples * 2];
    uint32_t sequenceNumber = 10;
    const auto framesInPacketBuffer = samples;

    auto opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);
    EXPECT_GT(opusBytes, 100);

    auto samplesProduced = decoder.decode(sequenceNumber,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);
    EXPECT_EQ(samplesProduced, samples);

    samplesProduced = decoder.decode(sequenceNumber + 9,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);
    EXPECT_EQ(samplesProduced, samples);

    samplesProduced = decoder.decode(sequenceNumber + 9,
        _opusData,
        opusBytes,
        reinterpret_cast<unsigned char*>(decodeBuffer),
        framesInPacketBuffer);
    EXPECT_EQ(samplesProduced, samples);

    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-27), -dB);
}
