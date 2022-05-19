#include "codec/G711codec.h"
#include "codec/AudioLevel.h"
#include "codec/PcmResampler.h"
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
        for (size_t i = 0; i < samples * 10; ++i)
        {
            _pcmData[i] = sin(2 * PI * i * 400 / sampleFrequency) * amplitude;
        }

        for (size_t i = 0; i < 10 * samples48; ++i)
        {
            _pcm48kHzData[i] = sin(2 * PI * i * 400 / 48000) * amplitude;
        }

        codec::PcmaCodec::initialize();
        codec::PcmuCodec::initialize();
    }

    static constexpr double PI = 3.14159;
    static constexpr double sampleFrequency = 8000;
    static constexpr size_t samples = sampleFrequency / 50;
    static constexpr double amplitude = 2000;
    int16_t _pcmData[samples * 10];

    static constexpr uint32_t samples48 = 48000 / 50;
    int16_t _pcm48kHzData[10 * samples48];
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

TEST_F(G711Test, upsampling)
{
    const auto inSamplesPerPacket = 8000 / 50;
    const auto outSamplesPerPacket = 48000 / 50;
    auto resampler = codec::createPcmResampler(outSamplesPerPacket, 8000, 48000);

    int16_t upsampled[outSamplesPerPacket];
    for (int i = 0; i < 3; ++i)
    {
        auto produced = resampler->resample(_pcmData + inSamplesPerPacket * i, inSamplesPerPacket, upsampled);
        EXPECT_EQ(produced, outSamplesPerPacket);
    }
}

TEST_F(G711Test, downsampling)
{
    const auto inSamplesPerPacket = 48000 / 50;
    const auto outSamplesPerPacket = 8000 / 50;
    auto resampler = codec::createPcmResampler(inSamplesPerPacket, 48000, 8000);

    int16_t downsampled[outSamplesPerPacket];
    for (int i = 0; i < 3; ++i)
    {
        auto produced = resampler->resample(_pcm48kHzData + inSamplesPerPacket * i, inSamplesPerPacket, downsampled);
        EXPECT_EQ(produced, outSamplesPerPacket);
    }
}
