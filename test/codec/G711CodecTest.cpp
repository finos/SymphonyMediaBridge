#include "codec/G711codec.h"
#include "codec/AudioLevel.h"
#include "logger/Logger.h"
#include "utils/Time.h"
#include <cmath>
#include <gtest/gtest.h>

namespace codec
{
int computeAudioLevel(const int16_t* payload, int samples);
}

struct G711Test : testing::Test
{
    void SetUp() override
    {
        for (size_t i = 0; i < samples; ++i)
        {
            _pcmData[i] = sin(2 * PI * i * 400 / sampleFrequency) * amplitude;
        }

        codec::PcmaCodec::initialize();
        codec::PcmuCodec::initialize();
    }

    static constexpr double PI = 3.14159;
    static constexpr double sampleFrequency = 8000;
    static constexpr size_t samples = sampleFrequency / 50;
    static constexpr double amplitude = 2000;
    int16_t _pcmData[samples];
};

TEST_F(G711Test, alaw)
{
    codec::PcmaCodec codec;

    auto dB = codec::computeAudioLevel(_pcmData, samples);

    auto g711 = reinterpret_cast<uint8_t*>(_pcmData);
    codec.encode(_pcmData, g711, samples);

    codec.decode(g711, _pcmData, samples);
    auto dB2 = codec::computeAudioLevel(_pcmData, samples);

    EXPECT_EQ(dB, dB2);
    EXPECT_EQ(static_cast<int>(27), dB);
}

TEST_F(G711Test, ulaw)
{
    codec::PcmuCodec codec;

    auto dB = codec::computeAudioLevel(_pcmData, samples);

    auto g711 = reinterpret_cast<uint8_t*>(_pcmData);
    codec.encode(_pcmData, g711, samples);

    codec.decode(g711, _pcmData, samples);
    auto dB2 = codec::computeAudioLevel(_pcmData, samples);

    EXPECT_EQ(dB, dB2);
    EXPECT_EQ(static_cast<int>(27), dB);
}
