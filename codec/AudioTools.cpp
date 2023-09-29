#include "codec/AudioTools.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace codec
{
void makeStereo(int16_t* data, size_t count)
{
    for (int i = count - 1; i >= 0; i--)
    {
        data[i * 2] = data[i];
        data[i * 2 + 1] = data[i];
    }
}

// compacts stereo pcm16 by dropping every 48th sample
// shrinks by 2%
// returns new length of vector
size_t compactStereo(int16_t* pcmData, size_t samples)
{
    size_t writeIndex = 0;
    for (size_t i = 0; i < samples; ++i)
    {
        pcmData[writeIndex] = pcmData[i * 2];
        pcmData[writeIndex + 1] = pcmData[i * 2 + 1];

        if ((i % 48) == 0)
        {
            ++i;
        }

        writeIndex += 2;
    }
    return writeIndex / 2;
}

void sineTail(int16_t* data, double freq, uint32_t sampleRate, size_t count)
{
    auto maxIt = std::max_element(data, data + count * 2);
    const double peak = std::abs(*maxIt);

    const double delta = 2 * M_PI * freq / sampleRate;
    double s = data[(count - 1) * 2];
    uint32_t downCount = 0;
    for (size_t i = count - 10; i < count; ++i)
    {
        if (data[i * 2] > s)
        {
            ++downCount;
        }
    }
    const double fqCut = 250.0;
    const double invSampleRate = 1.0 / sampleRate;
    const double m = (2 * M_PI * fqCut * invSampleRate);
    const double alpha = m / (1.0 + m);

    if (peak == 0)
    {
        codec::clearStereo(data, count);
        return;
    }

    double phase = 0;
    if (downCount > 5)
    {
        const double quadrant = (s >= 0) ? M_PI / 2 : M_PI;

        for (size_t t = 0; t < sampleRate * 4 / freq; ++t)
        {
            if (peak * sin(t * delta + quadrant) < s)
            {
                phase = quadrant + t * delta;
                break;
            }
        }
    }
    else
    {
        const double quadrant = (s >= 0) ? 0 : M_PI * 3 / 4;
        for (size_t t = 0; t < freq / 2; ++t)
        {
            if (peak * sin(t * delta + quadrant) > s)
            {
                phase = quadrant + t * delta;
                break;
            }
        }
    }

    for (size_t t = 0; t < count; ++t)
    {
        double v = peak * sin(t * delta + phase);

        s += alpha * (v - s);
        data[t * 2] = s;
        data[t * 2 + 1] = s;
    }
}

void swingTailMono(int16_t* data, const uint32_t sampleRate, const size_t count, const int step)
{
    const double tailFrequency = 250;
    const double m = 2.0 * M_PI * 2550.0 / sampleRate;
    const double alpha = m / (1.0 + m);
    double y[2] = {0};
    y[0] = data[(count - 51) * step];

    for (size_t i = count - 50; i < count; ++i)
    {
        y[1] = y[0];
        y[0] += alpha * (data[i * step] - y[0]);
    }

    double v = y[0] - y[1];
    const double beta = tailFrequency / (M_PI * sampleRate);
    double yn = data[(count - 1) * step];
    double amplification = 0.9;
    for (size_t i = 0; i < count; ++i)
    {
        yn = yn + v;
        v = (v - beta * yn);
        data[i * step] = yn * amplification;
        amplification *= 0.99;
    }
}

void swingTail(int16_t* data, const uint32_t sampleRate, const size_t count)
{
    swingTailMono(data, sampleRate, count, 2);
    swingTailMono(data + 1, sampleRate, count, 2);
}

void addToMix(const int16_t* srcAudio, int16_t* mixAudio, size_t count, double amplification)
{
    if (amplification == 1.0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            mixAudio[i] += srcAudio[i];
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        mixAudio[i] += amplification * srcAudio[i];
    }
}

void subtractFromMix(const int16_t* srcAudio, int16_t* mixAudio, size_t count, double amplification)
{
    for (size_t i = 0; i < count; ++i)
    {
        mixAudio[i] -= amplification * srcAudio[i];
    }
}

/**
 * Eliminate samples at times where energy is low which makes it less audible.
 */
size_t compactStereoTroughs(int16_t* pcmData,
    size_t samples,
    size_t maxReduction,
    const int16_t silenceThreshold,
    const int16_t deltaThreshold)
{
    pcmData[0] = pcmData[0];
    pcmData[1] = pcmData[1];
    size_t produced = 1;

    size_t removedSamples = 0;
    int keepSamples = 0;
    for (size_t i = 1; i < samples - 1; ++i)
    {
        const int16_t a = pcmData[i * 2 - 2];
        const int16_t c = pcmData[i * 2 + 2];

        if (removedSamples < maxReduction && !keepSamples && std::abs(a - c) < deltaThreshold)
        {
            if (std::abs(a) < silenceThreshold)
            {
                // low volume allow more sample removal
                keepSamples = 2;
            }
            else
            {
                keepSamples = 8;
            }
            ++removedSamples;
            continue;
        }
        keepSamples = std::max(0, keepSamples - 1);

        pcmData[produced * 2] = pcmData[i * 2];
        pcmData[produced * 2 + 1] = pcmData[i * 2 + 1];

        ++produced;
    }

    const size_t i = samples - 1;
    pcmData[produced * 2] = pcmData[i * 2];
    pcmData[produced * 2 + 1] = pcmData[i * 2 + 1];
    ++produced;

    return produced;
}
} // namespace codec
