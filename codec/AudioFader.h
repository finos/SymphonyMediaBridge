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

    void fadeInStereo(const int16_t* srcData, int16_t* outData, size_t sampleCount)
    {

        for (size_t i = 0; i < sampleCount; ++i)
        {
            _volume = std::min(1.0, _volume + _stepFactor);
            outData[i * 2] = srcData[i * 2] * _volume;
            outData[i * 2 + 1] = srcData[i * 2 + 1] * _volume;
        }
    }

    void fadeInStereo(int16_t* inOutData, size_t sampleCount)
    {
        return fadeInStereo(inOutData, inOutData, sampleCount);
    }

    void fadeOutStereo(const int16_t* srcData, int16_t* outData, size_t sampleCount)
    {
        for (size_t i = 0; i < sampleCount; ++i)
        {
            _volume = std::min(1.0, _volume + _stepFactor);
            const auto amp = 1.0 - _volume;
            outData[i * 2] = srcData[i * 2] * amp;
            outData[i * 2 + 1] = srcData[i * 2 + 1] * amp;
        }
    }

    void fadeOutStereo(int16_t* data, size_t sampleCount) { fadeOutStereo(data, data, sampleCount); }

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

} // namespace codec
