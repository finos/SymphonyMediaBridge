#pragma once

namespace math
{
template <typename T>
T clamp(const T& value, const T& minValue, const T& maxValue)
{
    return std::min(std::max(value, minValue), maxValue);
}

template <uint8_t mantissaBits, uint8_t exponentBits>
bool extractMantissaExponent(uint64_t value, uint32_t& mantissa, uint32_t& exponent)
{
    static_assert(mantissaBits <= 32, "mantissa too large");
    static_assert(exponentBits < 16, "exponent too large");

    exponent = 0;
    const uint64_t maxMantissa = (uint64_t(1) << mantissaBits) - 1;
    const uint32_t maxExponent = (1u << exponentBits) - 1;
    while (value & ~maxMantissa)
    {
        ++exponent;
        value >>= 1;
    }

    if (exponent > maxExponent)
    {
        return false;
    }
    mantissa = static_cast<uint32_t>(value);
    return true;
}

template <typename T>
T roundUpMultiple(T value, T N)
{
    const auto r = value % N;
    if (r > 0)
    {
        return value - r + N;
    }

    return value;
}
} // namespace math
