#include "ResampleFilters.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <immintrin.h>
#include <memory>
#include <numeric>

namespace codec
{
namespace
{
// generated with python scipy.signal.remez package.
// remex {0, 4000, 7000, 24000} to reduce ringing
alignas(32) const float coefficients48[] = {-0.0014312130547252334,
    -0.0006054419098336808,
    0.0005125525474648318,
    0.002265637857911059,
    0.003613691908452525,
    0.0032400421353444516,
    0.00042171822556735686,
    -0.004142286440019049,
    -0.008161844774923781,
    -0.008669699057834468,
    -0.0037526619271314693,
    0.005721817368260278,
    0.015574714928136658,
    0.019701079061089664,
    0.013157977574650844,
    -0.0043841116127715345,
    -0.026731687155170873,
    -0.04228380449900747,
    -0.038315549216148145,
    -0.006862934685613791,
    0.050462556270647566,
    0.12127833849315953,
    0.18591190718004647,
    0.2244508852158846,
    0.2244508852158846,
    0.18591190718004647,
    0.12127833849315953,
    0.050462556270647566,
    -0.006862934685613791,
    -0.038315549216148145,
    -0.04228380449900747,
    -0.026731687155170873,
    -0.0043841116127715345,
    0.013157977574650844,
    0.019701079061089664,
    0.015574714928136658,
    0.005721817368260278,
    -0.0037526619271314693,
    -0.008669699057834468,
    -0.008161844774923781,
    -0.004142286440019049,
    0.00042171822556735686,
    0.0032400421353444516,
    0.003613691908452525,
    0.002265637857911059,
    0.0005125525474648318,
    -0.0006054419098336808,
    -0.0014312130547252334};

// remez band {0,3400,4600,24000}
alignas(32) const float dec_coEfficients19[] = {-0.10167076519742091,
    -0.026316766959229757,
    -0.015019643734867387,
    0.0070442586114255286,
    0.038346323980530325,
    0.07507317680725231,
    0.1121844952091097,
    0.14418018600376672,
    0.16575542007242253,
    0.17322498623364782,
    0.16575542007242253,
    0.14418018600376672,
    0.1121844952091097,
    0.07507317680725231,
    0.038346323980530325,
    0.0070442586114255286,
    -0.015019643734867387,
    -0.026316766959229757,
    -0.10167076519742091};
} // namespace

// Pepares a 6 stage polyphase filter from the 48 taps
FirUpsample6x::FirUpsample6x() : _tapsPerLine(8)
{
    clear();
    for (auto i = 0u; i < _resampleFactor; ++i)
    {
        for (auto j = 0u; j < _tapsPerLine; ++j)
        {
            _filterLine[i][j] = coefficients48[i + j * _resampleFactor];
        }
    }
}

void FirUpsample6x::clear()
{
    _samples.fill(0.f);
}

// performs 8 * 48000 = 384000 mul/s to upsample 8kHz->48kHz
void FirUpsample6x::upsample(int16_t* pcm, int16_t* pcmOut, const size_t sampleCountIn)
{
    assert(sampleCountIn + _tapsPerLine < sizeof(_samples));

    for (auto i = 0u; i < sampleCountIn; ++i)
    {
        _samples[_tapsPerLine + i] = pcm[i];
    }

    for (auto fi = 0u; fi < _resampleFactor; ++fi)
    {
        size_t outIndex = fi;
        const float* filterLine = _filterLine[fi];

        for (auto i = 0u; i < sampleCountIn; ++i)
        {
            // this layout allows compiler to load filter line into xmm and vfmadd321
            const auto m = _tapsPerLine + i - 7;
            float ns = _samples[m] * filterLine[7];
            ns += _samples[m + 1] * filterLine[6];
            ns += _samples[m + 2] * filterLine[5];
            ns += _samples[m + 3] * filterLine[4];
            ns += _samples[m + 4] * filterLine[3];
            ns += _samples[m + 5] * filterLine[2];
            ns += _samples[m + 6] * filterLine[1];
            ns += _samples[m + 7] * filterLine[0];

            pcmOut[outIndex] = std::roundf(ns * _resampleFactor);
            outIndex += _resampleFactor;
        }
    }

    std::memcpy(_samples.data(), _samples.data() + sampleCountIn, _tapsPerLine * sizeof(float));
}

FirDownsample6x::FirDownsample6x()
{
    clear();
}

void FirDownsample6x::clear()
{
    std::fill(_samples, &_samples[MAX_TAPS], 0);
}

// Requires 8000 * 19 = 152000 mul/s to do 48k->8k
void FirDownsample6x::downsample(int16_t* pcm, int16_t* pcmOut, const size_t sampleCountIn)
{
    assert(sampleCountIn <= 1440);
    const size_t TAPS = 19;

    for (auto i = 0u; i < sampleCountIn; ++i)
    {
        _samples[TAPS + i] = pcm[i];
    }

    // compiler recognises that there are 10 unique taps and loads those into xmm registers.
    // then it performs vfmadd231ss to multiply and accumulate
    auto* pOut = pcmOut;
    for (auto i = 0u; i < sampleCountIn; i += _resampleFactor)
    {
        float ns = 0;
        for (auto j = 0u; j < TAPS; ++j)
        {
            ns += _samples[TAPS + i - j] * dec_coEfficients19[j];
        }
        *pOut = std::roundf(ns);
        ++pOut;
    }

    for (auto i = 0u; i < TAPS; ++i)
    {
        _samples[i] = pcm[sampleCountIn - TAPS + i];
    }
}
} // namespace codec
