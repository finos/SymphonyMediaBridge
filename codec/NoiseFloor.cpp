#include "codec/NoiseFloor.h"
#include "codec/AudioLevel.h"
#include <cmath>

namespace codec
{
void NoiseFloor::onPcm(int16_t* pcmData, size_t sampleCount)
{
    update(computeAudioLevel(pcmData, sampleCount));
}

void NoiseFloor::update(double audioLevel)
{
    if (audioLevel >= 127)
    {
        return;
    }

    if (audioLevel > _noiseLevel)
    {
        _noiseLevel = audioLevel;
    }
    else
    {
        _noiseLevel *= 0.999;
    }
}

double NoiseFloor::getLevel() const
{
    return _noiseLevel;
}
} // namespace codec
