#pragma once
#include <cstddef>
#include <cstdint>

namespace codec
{
class NoiseFloor
{
public:
    NoiseFloor() : _noiseLevel(0) {}

    void onPcm(int16_t* pcmData, size_t sampleCount);
    void update(double audioLevel);
    double getLevel() const;

private:
    double _noiseLevel;
};

} // namespace codec
