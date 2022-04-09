#pragma once
#include <array>
#include <type_traits>

namespace memory
{

template <typename T, size_t N>
class RandomAccessBacklog
{
    static_assert(((N - 1) & N) == 0, "Size must be a power of 2");
    template <typename D>
    class base_iterator
    {
    public:
        base_iterator(const RandomAccessBacklog<T, N>& backlog, uint32_t index)
            : _backlog(const_cast<RandomAccessBacklog<T, N>&>(backlog)),
              _index(index)
        {
        }

        D& operator*() { return _backlog[_index]; }
        D& operator->() { return _backlog[_index]; }

        base_iterator operator++(int i)
        {
            iterator iter(*this);
            ++(*this);
            return iter;
        }

        base_iterator& operator++()
        {
            ++_index;
            return *this;
        }

        bool operator==(const base_iterator<D>& a) { return a._index == _index; }
        bool operator!=(const base_iterator<D>& a) { return a._index != _index; }

    private:
        RandomAccessBacklog<T, N>& _backlog;
        uint32_t _index;
    };

public:
    typedef base_iterator<const T> const_iterator;
    typedef base_iterator<T> iterator;

    RandomAccessBacklog() : _head(0), _count(0) {}
    ~RandomAccessBacklog() { clear(); }

    inline T& operator[](uint32_t index) { return *reinterpret_cast<T*>(&_data[(_head + index) % N]); }
    inline const T& operator[](uint32_t index) const
    {
        return *reinterpret_cast<const T*>(&_data[(_head + index) % N]);
    }

    size_t size() const { return _count; }
    bool empty() const { return _count == 0; }
    bool full() const { return _count >= N; }
    size_t capacity() const { return N; }

    const_iterator cbegin() const { return const_iterator(*this, 0); }
    iterator begin() { return iterator(*this, 0); }
    const_iterator cend() const { return const_iterator(*this, size()); }
    iterator end() { return iterator(*this, size()); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }

    T& front() { return (*this)[0]; }
    const T& front() const { return (*this)[0]; }
    T& back() { return (*this)[_count - 1]; }
    const T& back() const { return (*this)[_count - 1]; }

    void push_front(const T& item)
    {
        --_head;
        if (size() == N)
        {
            reinterpret_cast<T*>(&_data[_head % N])->~T();
            --_count;
        }

        new (&_data[_head % N]) T(item);
        ++_count;
    }

    void push_front(T&& item)
    {
        --_head;
        if (size() == N)
        {
            reinterpret_cast<T*>(&_data[_head % N])->~T();
            --_count;
        }

        new (&_data[_head % N]) T(std::move(item));
        ++_count;
    }

    template <typename... Args>
    void emplace_front(Args&&... args)
    {
        --_head;
        if (size() == N)
        {
            reinterpret_cast<T*>(&_data[_head % N])->~T();
            --_count;
        }
        new (&_data[_head % N]) T(std::forward<Args>(args)...);
        ++_count;
    }

    void clear()
    {
        for (uint32_t i = 0; i < _count; ++i)
        {
            reinterpret_cast<T*>(&_data[(_head + i) % N])->~T();
        }
        _count = 0;
    }

    void pop_back()
    {
        if (_count > 0)
        {
            reinterpret_cast<T*>(&_data[(_head + _count - 1) % N])->~T();
            --_count;
        }
    }

    T fetchBack()
    {
        auto value = std::move(back());
        pop_back();
        return value;
    }

private:
    typename std::aligned_storage<sizeof(T), alignof(T)>::type _data[N];
    uint32_t _head;
    uint32_t _count;
};

} // namespace memory
