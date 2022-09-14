#pragma once
#include <cstddef>
#include <type_traits>

namespace math
{

// difference (b - a) mod (1<<BITS)
template <typename T, size_t BITS = sizeof(T) * 8>
constexpr typename std::make_signed<T>::type ringDifference(T a, const T b)
{
    using SignedT = typename std::make_signed<T>::type;
    static_assert(std::is_unsigned<T>::value, "Difference only works on unsigned integers");
    assert(sizeof(T) * 8 == BITS || a < (T(1) << BITS));
    assert(sizeof(T) * 8 == BITS || b < (T(1) << BITS));

    constexpr T mask = (T(0) - 1) >> (sizeof(T) * 8 - BITS);

    T v = (b - a) & mask;

    if (v > (mask >> 1))
    {
        return static_cast<SignedT>(v | ~mask);
    }

    return static_cast<SignedT>(v);
}

} // namespace math
