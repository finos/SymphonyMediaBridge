#pragma once
#include <cstdint>
#include <type_traits>

namespace memory
{
namespace detail
{

template <class T>
std::enable_if_t<std::is_pointer<std::decay_t<T>>::value, T> pointerOf(T&& value)
{
    return value;
}

template <class T>
std::enable_if_t<!std::is_pointer<std::decay_t<T>>::value, std::remove_reference_t<T>*> pointerOf(T&& value)
{
    return &value;
}

} // namespace detail

template <typename T>
bool isAligned(void* ptr)
{
    return reinterpret_cast<intptr_t>(ptr) % alignof(T) == 0;
}
} // namespace memory
