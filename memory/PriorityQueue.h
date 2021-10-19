#pragma once

#include "utils/ScopedInvariantChecker.h"
#include <array>
#include <cassert>

namespace memory
{

template <typename T, size_t MAX_ELEMENTS>
class PriorityQueue
{
public:
    PriorityQueue() : _nextFree(0)
    {
        static_assert((MAX_ELEMENTS & (MAX_ELEMENTS - 1)) == 0, "MAX_ELEMENTS must be a power of two");
        static_assert(std::is_trivial<T>(), "T must be trivial type");
        memset(_data.data(), 0, sizeof(T) * _data.size());
    }

    void push(const T& element)
    {
#if DEBUG
        utils::ScopedInvariantChecker<PriorityQueue> invariantChecker(*this);
#endif
        if (_nextFree >= MAX_ELEMENTS)
        {
            assert(false);
            return;
        }

        _data[_nextFree] = element;
        size_t i = _nextFree;
        ++_nextFree;
        if (i == 0)
        {
            return;
        }

        while (true)
        {
            const auto parent = getParent(i);
            const auto oldParentValue = _data[parent];
            const auto currentValue = _data[i];

            if (oldParentValue >= currentValue)
            {
                return;
            }

            _data[parent] = currentValue;
            _data[i] = oldParentValue;

            i = parent;
            if (i == 0)
            {
                return;
            }
        }
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
#if DEBUG
        utils::ScopedInvariantChecker<PriorityQueue> invariantChecker(*this);
#endif

        if (_nextFree == 0)
        {
            assert(false);
            return;
        }

        --_nextFree;
        _data[0] = _data[_nextFree];
        size_t i = 0;

        while (true)
        {
            auto largest = i;
            const auto leftChild = getLeftChild(i);
            const auto rightChild = getRightChild(i);

            if (leftChild < _nextFree && _data[leftChild] > _data[largest])
            {
                largest = leftChild;
            }

            if (rightChild < _nextFree && _data[rightChild] > _data[largest])
            {
                largest = rightChild;
            }

            if (largest == i)
            {
                return;
            }

            const auto oldValue = _data[largest];
            _data[largest] = _data[i];
            _data[i] = oldValue;
            i = largest;
        }
    }

    bool isEmpty() const { return _nextFree == 0; }

#if DEBUG
    bool isValidNode(const size_t index) const
    {
        const auto leftChild = getLeftChild(index);
        if (leftChild < _nextFree)
        {
            if (_data[index] >= _data[leftChild])
            {
                return isValidNode(leftChild);
            }
            else
            {
                return false;
            }
        }

        const auto rightChild = getRightChild(index);
        if (rightChild < _nextFree)
        {
            if (_data[index] >= _data[rightChild])
            {
                return isValidNode(rightChild);
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    void checkInvariant() const { assert(isValidNode(0)); }
#endif

private:
    std::array<T, MAX_ELEMENTS> _data;
    size_t _nextFree;

    constexpr size_t getParent(const size_t index) const
    {
        assert(index > 0);
        return (index - 1) / 2;
    }

    constexpr size_t getRightChild(const size_t index) const { return 2 * index + 2; }
    constexpr size_t getLeftChild(const size_t index) const { return 2 * index + 1; }
};

} // namespace memory
