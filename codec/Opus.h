#pragma once

#include <cstdint>

namespace codec
{

namespace Opus
{

constexpr uint32_t sampleRate = 48000;
constexpr uint32_t channelsPerFrame = 2;
constexpr uint32_t bytesPerSample = sizeof(int16_t);
constexpr uint32_t payloadType = 111;
constexpr uint32_t packetsPerSecond = 50; // default ptime

} // namespace Opus

} // namespace codec
