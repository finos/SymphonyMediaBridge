#pragma once

#include <array>
#include <iterator>
#include <type_traits>
namespace utils
{

/**
 * The class template span describes an object that can refer to a contiguous sequence of objects with the first element
 * of the sequence at position zero.
 *
 * This is intends to be an implementation of std::span as it only available on C++20.
 */
template <class T>
class Span
{
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    Span() noexcept : _first(nullptr), _last(nullptr) {}

    Span(pointer first, size_type count) : _first(first), _last(_first + count) {}

    Span(pointer first, pointer last) : _first(first), _last(last) {}

    template <class It>
    Span(It first, size_type count) : _first(first.operator->()),
                                      _last(_first + count)
    {
    }

    template <class It, class End>
    Span(It first, End last) : _first(first.operator->()),
                               _last(last.operator->())
    {
    }

    template <std::size_t N>
    constexpr Span(element_type (&arr)[N]) noexcept : _first(arr),
                                                      _last(_first + N)
    {
    }

    template <class U, std::size_t N>
    constexpr Span(std::array<U, N>& arr) noexcept : _first(arr.data()),
                                                     _last(_first + N)
    {
    }

    template <class U, std::size_t N>
    constexpr Span(const std::array<U, N>& arr) noexcept : _first(arr.data()),
                                                           _last(_first + N)
    {
    }

    const_iterator cbegin() const { return _first; }
    const_iterator cend() const { return _last; }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator begin() { return _first; }
    iterator end() { return _last; }

    const_reverse_iterator crbegin() const { return reverse_iterator(_last); }
    const_reverse_iterator crend() const { return reverse_iterator(_first); }
    const_reverse_iterator rbegin() const { return crbegin(); }
    const_reverse_iterator rend() const { return crend(); }
    reverse_iterator rbegin() { return reverse_iterator(_last); }
    reverse_iterator rend() { return reverse_iterator(_first); }

    const_reference front() const { return *_first; }
    const_reference back() const { return *(_last - 1); }
    reference front() { return *_first; }
    reference back() { return *(_last - 1); }
    reference operator[](size_type index) { return *(_first + index); }
    const_reference operator[](size_type index) const { return *(_first + index); }

    pointer data() { return _first; }
    const_pointer data() const { return _first; }

    size_t size() const { return _last - _first; }
    size_t empty() const { return _last == _first; }

    Span subSpan(size_t offset, size_t count) const { return Span(_first + offset, _first + offset + count); }
    Span subSpan(size_t offset) const { return Span(_first + offset, _last); }

private:
    pointer _first;
    pointer _last;
};

} // namespace utils
