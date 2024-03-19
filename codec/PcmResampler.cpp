#include "PcmResampler.h"
#include "SpeexResampler.h"
#include "logger/Logger.h"
#include <algorithm>

namespace codec
{

class PcmResamplerImpl : public PcmResampler
{
public:
    PcmResamplerImpl(uint32_t maxSamplesIn, uint32_t rateIn, uint32_t rateOut, uint16_t quality)
        : _ratio(static_cast<float>(rateOut) / std::max(1u, rateIn)),
          _inSamples(new float[maxSamplesIn]),
          _outSamples(new float[1 + maxSamplesIn * _ratio]),
          _maxSamplesIn(maxSamplesIn)
    {
        _resampler.init(1, rateIn, rateOut, quality, nullptr);
    }

    ~PcmResamplerImpl()
    {
        delete[] _inSamples;
        delete[] _outSamples;
    }

    size_t resample(const int16_t* pcmDataIn, size_t samples, int16_t* target) override
    {
        if (!_inSamples || !_outSamples || samples > _maxSamplesIn)
        {
            return 0;
        }

        for (size_t i = 0; i < samples; ++i)
        {
            _inSamples[i] = static_cast<float>(pcmDataIn[i]);
        }

        speexport::spx_uint32_t count = samples;
        speexport::spx_uint32_t produced = samples * _ratio;
        auto result = _resampler.process(0, _inSamples, &count, _outSamples, &produced);
        if (result != speexport::RESAMPLER_ERR_SUCCESS)
        {
            return 0;
        }

        for (uint32_t i = 0; i < produced; ++i)
        {
            target[i] = lroundf(_outSamples[i]);
        }

        return produced;
    }

private:
    const float _ratio;
    float* _inSamples;
    float* _outSamples;
    const uint32_t _maxSamplesIn;

    speexport::SpeexResampler _resampler;
};

std::unique_ptr<PcmResampler> createPcmResampler(uint32_t maxSamples,
    uint32_t rateIn,
    uint32_t rateOut,
    uint16_t quality)
{
    return std::make_unique<PcmResamplerImpl>(maxSamples, rateIn, rateOut, quality);
}
} // namespace codec
