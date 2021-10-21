#pragma once
#include <array>

namespace memory
{

template <typename T, size_t S>
class RandomAccessBacklog
{
    static_assert(((S - 1) & S) == 0, "Size must be a power of 2");

public:
    RandomAccessBacklog() : _head(0), _tail(0) {}

    inline T& operator[](uint32_t index) { return _data[index % _data.size()]; }
    inline const T& operator[](uint32_t index) const { return _data[index % _data.size()]; }

    size_t size() const { return _tail - _head; }
    bool empty() const { return _tail == _head; }

    uint32_t tailIndex() const
    {
        if (empty())
        {
            return _tail;
        }
        return (_tail - 1);
    }

    uint32_t headIndex() const { return _head; }
    T& tail()
    {
        if (size() > 0)
        {
            return (*this)[tailIndex()];
        }
        return _data[_tail % _data.size()];
    }

    void push_back(const T& item)
    {
        if (size() == _data.size())
        {
            _data[_head++ % _data.size()].~T();
        }
        _data[_tail++ % _data.size()] = item;
    }

    template <typename... Args>
    void emplace_back(Args&&... args)
    {
        if (size() == _data.size())
        {
            _data[_head++ % _data.size()].~T();
        }
        new (&_data[_tail++ % _data.size()]) T(std::forward<Args>(args)...);
    }

    void clear()
    {
        for (auto i = _head; i < _tail; ++i)
        {
            _data[i].~T();
        }
        _head = 0;
        _tail = 0;
    }

private:
    std::array<T, S> _data;
    uint32_t _head;
    uint32_t _tail;
};

} // namespace memory
