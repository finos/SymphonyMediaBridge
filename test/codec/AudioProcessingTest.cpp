#include "codec/AudioLevel.h"
#include "logger/Logger.h"
#include <cmath>
#include <gtest/gtest.h>

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