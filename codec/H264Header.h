#pragma once

#include <arpa/inet.h>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace codec::H264Header
{

constexpr uint8_t getNalUnitType(const uint8_t nalHeader)
{
    return nalHeader & 0x1f;
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
    case 24: // STAP-A
    {
        size_t offset = 1;
        while (offset + 3 <= payloadSize)
        {
            const uint16_t naluSize = payload[offset] | (payload[offset + 1] << 8);
            offset += 2;
            if (getNalUnitType(payload[offset]) == 7)
            {
                return true;
            }
            offset += 1 + ntohs(naluSize);
        }
        return false;
    }
    case 28: // FU-A
    case 29: // FU-B
        return getNalUnitType(payload[1]) == 7 && (payload[1] & 0x80) != 0;
    default:
        return false;
    }
}

} // namespace codec::H264Header
