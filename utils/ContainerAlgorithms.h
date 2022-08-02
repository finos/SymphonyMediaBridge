#pragma once
#include <algorithm>

namespace utils
{
template <typename T, class UnaryPredicate>
bool contains(const T& container, UnaryPredicate predicate)
{
    return std::find_if(container.cbegin(), container.cend(), predicate) != container.cend();
}

template <typename T>
void append(T& target, const T& src)
{
    target.insert(target.begin(), src.begin(), src.end());
}

template <typename ContainerType, typename T, size_t N>
void append(ContainerType& target, const T (&src)[N])
{
    target.insert(target.begin(), src, &src[N]);
}
} // namespace utils
