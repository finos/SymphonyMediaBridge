#pragma once

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
        static_assert((MAX_ELEMENTS & (MAX_ELEMENTS - 1)) == 0, "MAX_ELEMENTS must be a power of two");
        memset(_data.data(), 0, sizeof(T) * _data.size());
    }

    void push(const T& element)
    {
        if (_nextFree >= MAX_ELEMENTS)
        {
            assert(false);
            return;
        }

        _data[_nextFree] = element;
        size_t i = _nextFree;
        ++_nextFree;

        while (i != 0)
        {
            const auto parent = getParent(i);
            if (_data[parent] >= _data[i])
            {
                return;
            }

            const auto oldParentValue = _data[parent];
            _data[parent] = _data[i];
            _data[i] = oldParentValue;

            i = parent;
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
    void clear() { _nextFree = 0; }

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
