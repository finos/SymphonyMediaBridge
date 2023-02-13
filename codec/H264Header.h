#pragma once

#include <arpa/inet.h>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace codec
{

namespace H264Header
{

constexpr uint8_t getNalUnitType(const uint8_t nalHeader)
{
    return nalHeader & 0x1f;
}

constexpr uint8_t getFuNalStartBit(const uint8_t fuHeader)
{
    return fuHeader & 0x80;
}

inline bool isKeyFrameStapA(const uint8_t* payload, const size_t payloadSize)
{
    size_t offset = 1;

    while (payloadSize - offset >= 3)
    {
        const uint16_t naluSize = *reinterpret_cast<const uint16_t*>(&payload[offset]);
        offset += 2;
        if (getNalUnitType(payload[offset]) == 7)
        {
            return true;
        }
        offset += 1 + ntohs(naluSize);
    }
    return false;
}

inline bool isKeyFrame(const uint8_t* payload, const size_t payloadSize)
{
    if (payloadSize <= 1)
    {
        return false;
    }

    switch (getNalUnitType(payload[0]))
    {
    case 7:
        return true;
    case 24:
        return isKeyFrameStapA(payload, payloadSize);
    case 28:
    case 29:
        return getNalUnitType(payload[1]) == 7 && getFuNalStartBit(payload[1]) != 0;
    default:
        return false;
    }
}

} // namespace H264

} // namespace codec
