#pragma once

#include <algorithm>
#include <array>
#include <cassert>

namespace memory
{

template <typename T, size_t MAX_ELEMENTS>
class PriorityQueue
{
public:
    explicit PriorityQueue() : _nextFree(0)
    {
        static_assert(std::is_trivial<T>(), "T must be a trivial type");
        static_assert((MAX_ELEMENTS & (MAX_ELEMENTS - 1)) == 0, "MAX_ELEMENTS must be a power of two");
    }

    void push(const T& element)
    {
        if (_nextFree >= MAX_ELEMENTS)
        {
            assert(false);
            return;
        }

        _data[_nextFree] = element;
        ++_nextFree;
        std::push_heap<>(_data.begin(), _data.begin() + _nextFree);
    }

    const T& top() const
    {
        if (_nextFree == 0)
        {
            assert(false);
        }

        return _data[0];
    }

    void pop()
    {
        if (_nextFree == 0)
        {
            assert(false);
            return;
        }

        std::pop_heap(_data.begin(), _data.begin() + _nextFree);
        --_nextFree;
    }

    bool empty() const { return _nextFree == 0; }
    void clear() { _nextFree = 0; }

private:
    std::array<T, MAX_ELEMENTS> _data;
    size_t _nextFree;
};

} // namespace memory
