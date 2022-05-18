#include "PcmUtils.h"

namespace codec
{

void makeStereo(int16_t* data, size_t samples)
{
    for (size_t i = 1; i <= samples; ++i)
    {
        data[2 * (samples - i) + 1] = data[samples - i];
        data[2 * (samples - i)] = data[samples - i];
    }
}

void makeMono(int16_t* data, size_t samples)
{
    for (size_t i = 0; i < samples; ++i)
    {
        data[i] = data[2 * i];
    }
}

} // namespace codec
