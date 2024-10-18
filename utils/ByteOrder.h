#pragma once
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cstdint>

inline bool isBigEndian() noexcept
{
    const uint32_t test = 1;
    return reinterpret_cast<const char*>(&test) == 0;
}

template <typename T>
T hton(T value) noexcept
{
    if (!isBigEndian())
    {
        char* ptr = reinterpret_cast<char*>(&value);
        std::reverse(ptr, ptr + sizeof(T));
    }
    return value;
}

template <typename T>
void hton(const T& value, T& dst) noexcept
{
    assert(&value != &dst);
    if (!isBigEndian())
    {
        assert((intptr_t)(&dst) % alignof(T) == 0);
        auto v = reinterpret_cast<const char*>(&value);
        std::reverse_copy(v, v + sizeof(T), reinterpret_cast<char*>(&dst));
    }
    else
    {
        std::memcpy(&dst, &value, sizeof(T));
    }
}

template <typename T>
T ntoh(T value) noexcept
{
    return hton(value);
}

template <typename IntT>
class NetworkOrdered
{
    IntT _value;

public:
    NetworkOrdered() { std::memset(&_value, 0, sizeof(IntT)); }
    NetworkOrdered(const NetworkOrdered&) = default;
    explicit NetworkOrdered(const IntT& v) { hton(v, _value); }

    operator IntT() const { return get(); }

    NetworkOrdered& operator=(const NetworkOrdered<IntT>& v) = default;
    NetworkOrdered& operator=(const IntT& v)
    {
        assert((intptr_t)(&_value) % alignof(IntT) == 0);
        hton(v, _value);
        return *this;
    }

    NetworkOrdered& operator+=(const IntT& v)
    {
        assert((intptr_t)(&_value) % alignof(IntT) == 0);
        IntT current = ntoh<IntT>(_value) + v;
        hton(current, _value);
        return *this;
    }

    IntT get() const
    {
        assert((intptr_t)(&_value) % alignof(IntT) == 0);
        return ntoh(_value);
    }
};

typedef NetworkOrdered<uint64_t> nwuint64_t;
typedef NetworkOrdered<uint32_t> nwuint32_t;
typedef NetworkOrdered<uint16_t> nwuint16_t;

template <typename IntT>
IntT toLittleEndian(IntT value)
{
    if (isBigEndian())
    {
        char* ptr = reinterpret_cast<char*>(&value);
        std::reverse(ptr, ptr + sizeof(IntT));
    }

    return value;
}
