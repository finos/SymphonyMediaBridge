#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace memory
{

class AudioPacket;
class Packet;

} // namespace memory

namespace codec
{
class AudioFilter;

template <typename T>
void makeStereo(T* data, size_t count)
{
    for (int i = count - 1; i >= 0; i--)
    {
        data[i * 2] = data[i];
        data[i * 2 + 1] = data[i];
    }
}

size_t compactStereo(int16_t* pcmData, size_t size);
size_t compactStereoSlope(int16_t* pcmData, size_t samples, size_t maxReduction = 1000);

template <typename T>
void clearStereo(T* data, size_t count)
{
    std::memset(data, 0, count * 2 * sizeof(T));
}

template <typename T>
void copyStereo(const T* srcData, T* data, size_t count)
{
    std::memcpy(data, srcData, count * 2 * sizeof(T));
}

void sineTail(int16_t* data, double freq, uint32_t sampleRate, size_t count);

void addToMix(const int16_t* srcAudio, int16_t* mixAudio, size_t count, double amplification);
void subtractFromMix(const int16_t* srcAudio, int16_t* mixAudio, size_t count, double amplification);

} // namespace codec
