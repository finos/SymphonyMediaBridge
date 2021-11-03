#include "codec/AudioLevel.h"
#include "codec/OpusDecoder.h"
#include "codec/OpusEncoder.h"
#include "logger/Logger.h"
#include <cmath>
#include <gtest/gtest.h>

namespace codec
{
int computeAudioLevel(const int16_t* payload, int samples);
}

struct OpusTest : testing::Test
{
    void SetUp() override
    {
        for (int i = 0; i < samples; ++i)
        {
            _pcmData[i * 2] = sin(2 * PI * i * 400 / sampleFrequency) * amplitude;
            _pcmData[i * 2 + 1] = _pcmData[i * 2];
        }
    }

    static constexpr double PI = 3.14159;
    static constexpr double sampleFrequency = 48000;
    static constexpr int samples = sampleFrequency / 50;
    static constexpr double amplitude = 2000;
    int16_t _pcmData[samples * 2];
    uint8_t _opusData[samples];
};

TEST_F(OpusTest, encode)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;

    auto opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);
    EXPECT_GT(opusBytes, 100);

    uint32_t bytesProduced = 0;
    auto decodeResult = decoder.decode(_opusData, opusBytes, 5, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_FALSE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    EXPECT_FALSE(decoder.decode(_opusData, opusBytes, 5, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced));
    EXPECT_EQ(bytesProduced, 0);

    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-31), -dB);
}

TEST_F(OpusTest, healing)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;

    auto opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);
    EXPECT_GT(opusBytes, 100);

    uint32_t bytesProduced = 0;
    auto decodeResult = decoder.decode(_opusData, opusBytes, 5, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_FALSE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    decodeResult = decoder.decode(_opusData, opusBytes, 8, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_TRUE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));
    decodeResult = decoder.decode(_opusData, opusBytes, 8, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_TRUE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));
    decodeResult = decoder.decode(_opusData, opusBytes, 8, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_FALSE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-25), -dB);
}

TEST_F(OpusTest, seqSkip)
{
    codec::OpusEncoder encoder;
    codec::OpusDecoder decoder;

    auto opusBytes = encoder.encode(_pcmData, samples, _opusData, samples);
    EXPECT_GT(opusBytes, 100);

    uint32_t bytesProduced = 0;
    auto decodeResult = decoder.decode(_opusData, opusBytes, 5, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_FALSE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    decodeResult = decoder.decode(_opusData, opusBytes, 16, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_TRUE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    decodeResult = decoder.decode(_opusData, opusBytes, 16, samples * 2 * sizeof(int16_t), _pcmData, bytesProduced);
    EXPECT_FALSE(decodeResult);
    EXPECT_EQ(bytesProduced, samples * 2 * sizeof(int16_t));

    auto dB = codec::computeAudioLevel(_pcmData, samples);
    EXPECT_EQ(static_cast<int>(-24), -dB);
}
