#pragma once
#include <algorithm>

namespace utils
{
template <typename T, class UnaryPredicate>
bool contains(const T& container, UnaryPredicate predicate)
{
    return std::find_if(container.cbegin(), container.cend(), predicate) != container.cend();
}
} // namespace utils