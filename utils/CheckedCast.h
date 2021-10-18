#pragma once

#include <cassert>
#include <cstdint>
#include <limits>

namespace utils
{

template <typename TO, typename FROM>
constexpr TO checkedCast(FROM value)
{
#ifdef DEBUG
    if (std::is_signed<TO>())
    {
        assert(value <= std::numeric_limits<TO>::max());
        if (std::is_signed<FROM>())
        {
            assert(value >= std::numeric_limits<TO>::min());
        }
    }
    else
    {
        assert(value >= 0);
        assert(static_cast<TO>(value) <= std::numeric_limits<TO>::max());
    }
#endif

    return static_cast<TO>(value);
}

template <>
constexpr long checkedCast<long, uint32_t>(uint32_t value)
{
#ifdef DEBUG
    const auto maxValue = std::numeric_limits<long>::max();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
    assert(value <= maxValue);
#pragma clang diagnostic pop
#endif
    return static_cast<long>(value);
}

} // namespace utils
