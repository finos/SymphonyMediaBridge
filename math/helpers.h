#pragma once

namespace math
{
template <typename T>
T clamp(const T& value, const T& minValue, const T& maxValue)
{
    return std::min(std::max(value, minValue), maxValue);
}
} // namespace math
