#pragma once
#include <cstddef>
#include <type_traits>

namespace math
{

// difference (b - a) mod (1<<BITS)
template <typename T, size_t BITS>
constexpr typename std::make_signed<T>::type ringDifference(T a, const T b)
{
    static_assert(std::is_unsigned<T>::value, "Difference only works on unsigned integers");
    using SignedT = typename std::make_signed<T>::type;
    if (a & (1 << (BITS - 1)))
    {
        // a has sign bit set. Extend it to negative number in larger ring
        a = a | (uint64_t(~0) << BITS);
        return static_cast<SignedT>(b - a);
    }
    else
    {
        return static_cast<SignedT>(b - a);
    }
}

} // namespace math
