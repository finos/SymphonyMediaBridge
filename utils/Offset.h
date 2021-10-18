#pragma once

#include <cstddef>
#include <cstdint>

namespace utils
{

namespace Offset
{

template <typename T, size_t BITS>
constexpr T getOffset(const T first, const T second)
{
    auto result = first - second;

    if (result < -(1 << (BITS - 1)))
    {
        result += 1 << BITS;
    }
    else if (result > (1 << (BITS - 1)))
    {
        result -= 1 << BITS;
    }
    return result;
}

template <>
constexpr int64_t getOffset<int64_t, 32>(const int64_t first, const int64_t second)
{
    auto result = first - second;

    if (result < -(int64_t(1) << 31))
    {
        result += int64_t(1) << 32;
    }
    else if (result > (int64_t(1) << 31))
    {
        result -= int64_t(1) << 32;
    }
    return result;
}

} // namespace Offset

} // namespace utils
