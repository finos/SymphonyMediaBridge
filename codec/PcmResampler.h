#pragma once
#include <cstdint>
#include <memory>

namespace codec
{

class PcmResampler
{
public:
    virtual ~PcmResampler(){};

    /** pcmData and target may overlap */
    virtual size_t resample(const int16_t* pcmDataIn, size_t samplesIn, int16_t* target) = 0;
};

/**
 * quality 0-10. 0 = very poor
 */
std::unique_ptr<PcmResampler> createPcmResampler(uint32_t maxSamplesIn,
    uint32_t rateIn,
    uint32_t rateOut,
    uint16_t quality = 3);
} // namespace codec
