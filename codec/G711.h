#pragma once

#include <cstdint>

namespace codec
{

namespace Pcma
{

constexpr uint32_t sampleRate = 8000;
constexpr uint32_t channelsPerFrame = 1;
constexpr uint32_t bytesPerSample = sizeof(int8_t);
constexpr uint32_t payloadType = 8;
constexpr uint32_t packetsPerSecond = 50; // default ptime

} // namespace Pcma

namespace Pcmu
{

constexpr uint32_t sampleRate = 8000;
constexpr uint32_t channelsPerFrame = 1;
constexpr uint32_t bytesPerSample = sizeof(int8_t);
constexpr uint32_t payloadType = 0;
constexpr uint32_t packetsPerSecond = 50; // default ptime

} // namespace Pcmu

} // namespace codec
