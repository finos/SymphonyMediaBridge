#pragma once
#include <cstdint>
#include <math.h>

namespace codec
{
class AudioFadeIn
{
public:
    AudioFadeIn(uint32_t rampupDurationInSamples) : _stepFactor(pow(2.0, 15.0 / rampupDurationInSamples)), _volume(1.0)
    {
    }

    double next()
    {
        if (_volume >= 1 << 15)
        {
            return 1.0;
        }
        _volume = _volume * _stepFactor;
        return _volume / (1 << 15);
    }

    void reset() { _volume = 1.0; }
    double get() const { return _volume / (1 << 15); }

private:
    const double _stepFactor;
    double _volume;
};

class AudioFadeOut
{
public:
    AudioFadeOut(uint32_t rampupDurationInSamples)
        : _stepFactor(pow(2.0, 15.0 / rampupDurationInSamples)),
          _volume(1 << 15)
    {
    }

    double next()
    {
        if (_volume <= 0.0)
        {
            return 1.0;
        }
        _volume = _volume / _stepFactor;
        return _volume / (1 << 15);
    }

    void reset() { _volume = 1 << 15; }
    double get() const { return _volume / (1 << 15); }

private:
    const double _stepFactor;
    double _volume;
};

/**
 * Cross fade with energy preservation
 */
class AudioCrossFadeRms
{
public:
    AudioCrossFadeRms(uint32_t rampupDurationInSamples)
        : _x(0),
          _duration(rampupDurationInSamples),
          _fadeIn(0.0),
          _fadeOut(1.0),
          _sqrt2(std::sqrt(2.0))
    {
    }

    void next()
    {
        if (_x > _duration)
        {
            return;
        }
        const double x = _x / _duration;
        const double compl1X = 1.0 - x;
        const double xCompl1X = x * compl1X;
        const double baseFactor = xCompl1X * (1.0 + _sqrt2 * xCompl1X);
        const double fadeIn = baseFactor + x;
        const double fadeOut = baseFactor + compl1X;

        _fadeIn = fadeIn * fadeIn;
        _fadeOut = fadeOut * fadeOut;

        ++_x;
    }

    void reset()
    {
        _x = 0;
        _fadeIn = 0;
        _fadeOut = 1.0;
    }
    double getFadeIn() const { return _fadeIn; }
    double getFadeOut() const { return _fadeOut; }

private:
    uint32_t _x;
    const double _duration;
    double _fadeIn;
    double _fadeOut;
    const double _sqrt2;
};

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
