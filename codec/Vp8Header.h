#pragma once

#include <cstddef>
#include <cstdint>

namespace codec
{

namespace Vp8Header
{

constexpr uint8_t getX(const uint8_t* payload)
{
    return (payload[0] >> 0x7) & 0x1;
}

constexpr uint8_t getI(const uint8_t* payload)
{
    return (payload[1] >> 0x7) & 0x1;
}

constexpr uint8_t getL(const uint8_t* payload)
{
    return (payload[1] >> 0x6) & 0x1;
}

constexpr uint8_t getT(const uint8_t* payload)
{
    return (payload[1] >> 0x5) & 0x1;
}

constexpr uint8_t getK(const uint8_t* payload)
{
    return (payload[1] >> 0x4) & 0x1;
}

constexpr size_t getPayloadDescriptorSize(const uint8_t* payload, const size_t payloadSize)
{
    if (payloadSize == 0)
    {
        return 0;
    }

    size_t size = 1;

    const auto x = getX(payload);
    if (x == 0x0)
    {
        return size;
    }

    if (payloadSize < 2)
    {
        return 0;
    }

    const auto i = getI(payload);
    const auto l = getL(payload);
    const auto t = getT(payload);
    const auto k = getK(payload);

    size += 1;

    if (i == 0x1)
    {
        if (payloadSize < 3)
        {
            return 0;
        }

        const auto m = (payload[size] >> 0x7) & 0x1;
        if (m == 0x1)
        {
            size += 2;
        }
        else
        {
            size += 1;
        }
    }

    if (l == 0x1)
    {
        size += 1;
    }

    if (t == 0x1 || k == 0x1)
    {
        size += 1;
    }

    if (payloadSize < size)
    {
        return 0;
    }

    return size;
}

constexpr bool isStartOfPartition(const uint8_t* payload)
{
    const auto s = (payload[0] >> 0x4) & 0x1;
    return s == 0x1;
}

constexpr uint8_t getPartitionId(const uint8_t* payload)
{
    return payload[0] & 0xF;
}

constexpr uint8_t getTid(const uint8_t* payload)
{
    if (getPayloadDescriptorSize(payload, 6) != 6)
    {
        return 0xFF;
    }
    return (payload[5] >> 0x6) & 0x3;
}

constexpr uint16_t getPicId(const uint8_t* payload)
{
    if (getPayloadDescriptorSize(payload, 6) != 6)
    {
        return 0xFFFF;
    }
    return (static_cast<uint16_t>(payload[2] & 0x7F) << 0x8) | static_cast<uint16_t>(payload[3]);
}

constexpr uint8_t getTl0PicIdx(const uint8_t* payload)
{
    if (getPayloadDescriptorSize(payload, 6) != 6)
    {
        return 0xFF;
    }
    return payload[4];
}

constexpr void setPicId(uint8_t* payload, const uint16_t picId)
{
    if (getPayloadDescriptorSize(payload, 6) != 6)
    {
        return;
    }

    payload[2] = 0x80 | ((picId & 0x7F00) >> 8);
    payload[3] = picId & 0xFF;
}

constexpr void setTl0PicIdx(uint8_t* payload, uint8_t tl0PixIdx)
{
    if (getPayloadDescriptorSize(payload, 6) != 6)
    {
        return;
    }

    payload[4] = tl0PixIdx;
}

constexpr bool isKeyFrame(const uint8_t* payload, const size_t payloadDescriptorSize)
{
    if (!isStartOfPartition(payload) || getPartitionId(payload) != 0 || payloadDescriptorSize == 0)
    {
        return false;
    }
    return (payload[payloadDescriptorSize] & 0x1) == 0x0;
}

} // namespace Vp8Header

} // namespace codec
