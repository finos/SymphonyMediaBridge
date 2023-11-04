#pragma once
#include <array>
#include <cinttypes>
#include <cstddef>

namespace codec
{

class FirUpsample6x
{
    static constexpr size_t _resampleFactor = 6;

public:
    FirUpsample6x();

    void clear();
    void upsample(int16_t* pcm, int16_t* pcmOut, const size_t sampleCountIn);

private:
    const size_t _tapsPerLine;

    float _filterLine[_resampleFactor][8];
    std::array<float, 256> _samples;
};

class FirDownsample6x
{
    static constexpr size_t MAX_TAPS = 64;
    static constexpr size_t _resampleFactor = 6;

public:
    FirDownsample6x();

    void clear();
    void downsample(int16_t* pcm, int16_t* pcmOut, const size_t sampleCountIn);

private:
    alignas(32) float _samples[MAX_TAPS + 1440];
};

} // namespace codec
