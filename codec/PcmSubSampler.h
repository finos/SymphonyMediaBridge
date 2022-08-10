#pragma once
#include <cstdint>
#include <memory>

namespace codec
{

/**
 * Fast but filterless down sampler. Very efficient but can be problematic if there are alias frequencies in the
 * input.
 */
class PcmSubSampler
{
public:
    virtual ~PcmSubSampler();

    /** pcmData and target may overlap */
    virtual size_t process(const int16_t* pcmData, int16_t* target, size_t samples);
};

std::unique_ptr<PcmResampler> createPcmResampler(uint32_t maxSamples, uint32_t rateIn, uint32_t rateOut);
} // namespace codec
