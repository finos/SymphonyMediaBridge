#pragma once
#include <cstdint>
#include <math.h>

namespace codec
{

// fades in
class AudioLinearFade
{
public:
    AudioLinearFade(uint32_t rampDurationInSamples) : _stepFactor(1.0 / rampDurationInSamples), _volume(0) {}

    double next()
    {
        if (_volume >= 1.0)
        {
            return 1.0;
        }
        _volume += _stepFactor;
        return _volume;
    }

    void reset() { _volume = 0; }
    double get() const { return _volume; }

private:
    const double _stepFactor;
    double _volume;
};

// can work in place buffer
template <typename T, typename FADER>
void fadeInStereo(const T* srcData, T* outData, size_t sampleCount, FADER& fader)
{
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const auto amp = fader.next();
        outData[i * 2] = srcData[i * 2] * amp;
        outData[i * 2 + 1] = srcData[i * 2 + 1] * amp;
    }
}

template <typename T, typename FADER>
void fadeInStereo(T* inOutData, size_t sampleCount, FADER& fader)
{
    return fadeInStereo(inOutData, inOutData, sampleCount, fader);
}

template <typename T, typename FADER>
void fadeOutStereo(const T* srcData, T* outData, size_t sampleCount, FADER& fader)
{
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const auto amp = 1.0 - fader.next();
        outData[i * 2] = srcData[i * 2] * amp;
        outData[i * 2 + 1] = srcData[i * 2 + 1] * amp;
    }
}

template <typename T, typename FADER>
void fadeOutStereo(T* data, size_t sampleCount, FADER& fader)
{
    fadeOutStereo(data, data, sampleCount, fader);
}
} // namespace codec
