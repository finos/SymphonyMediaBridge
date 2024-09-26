#include "codec/ResampleFilters.h"
#include "utils/Format.h"
#include "utils/ScopedFileHandle.h"
#include <cmath>
#include <gtest/gtest.h>
#include <iostream>

namespace codec
{
int computeAudioLevel(const int16_t* payload, int samples);
}

TEST(AudioProcess, audiolevel)
{
    const double PI = 3.14159;
    const double sampleFrequency = 48000;
    const int samples = sampleFrequency / 50;
    int16_t data[samples * 2];
    const double amplitude = 2000;

    for (int i = 0; i < samples; ++i)
    {
        data[i * 2] = sin(2 * PI * i * 400 / sampleFrequency) * amplitude;
        data[i * 2 + 1] = data[i * 2];
    }
    auto dB = codec::computeAudioLevel(data, samples);
    double dBrms = 20 * std::log10(amplitude / (double(0x8000) * std::sqrt(2)));
    EXPECT_EQ(static_cast<int>(dBrms), -dB);
}

TEST(AudioProcess, DISABLED_upsample)
{
    utils::ScopedFileHandle inFile(::fopen("../tools/testfiles/jpsample.raw", "r"));
    utils::ScopedFileHandle outFile(::fopen("pcm48k48.raw", "wr"));

    codec::FirUpsample6x resampler;
    codec::FirDownsample6x downSampler;

    int16_t samples[960];
    int16_t samples8k[160];
    int16_t samples48k[960];
    for (auto i = 0u; i < 500; ++i)
    {
        ::fread(samples, sizeof(int16_t), 960, inFile.get());
        downSampler.downsample(samples, samples8k, 960);
        resampler.upsample(samples8k, samples48k, 160);

        ::fwrite(samples48k, sizeof(int16_t), 960, outFile.get());
    }
}

TEST(AudioProcess, DISABLED_downsample)
{
    utils::ScopedFileHandle inFile(::fopen("../tools/testfiles/jpsample.raw", "r"));
    utils::ScopedFileHandle outFile(::fopen("pcm8k19.raw", "wr"));

    codec::FirDownsample6x downSampler;

    int16_t samples[160];
    int16_t samples48k[960];
    for (auto i = 0u; i < 500; ++i)
    {
        ::fread(samples48k, sizeof(int16_t), 960, inFile.get());
        downSampler.downsample(samples48k, samples, 960);

        ::fwrite(samples, sizeof(int16_t), 160, outFile.get());
    }
}

TEST(AudioProcess, downsamplePerf)
{
    int16_t samples48k[960];

    codec::FirDownsample6x downSampler;
    int16_t samples8k[160];
    // 10h of audio to down sample
    for (auto i = 0u; i < 50 * 60 * 60 * 10; ++i)
    {
        downSampler.downsample(samples48k, samples8k, 960);
    }
}

TEST(AudioProcess, upsamplePerf)
{
    int16_t samples48k[960];

    codec::FirUpsample6x resampler;
    int16_t samples8k[160];
    // 10h of audio to down sample
    for (auto i = 0u; i < 50 * 60 * 60 * 10; ++i)
    {
        resampler.upsample(samples8k, samples48k, 160);
    }
}
