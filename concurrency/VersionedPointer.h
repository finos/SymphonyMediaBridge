#pragma once
#include <cassert>
#include <inttypes.h>
namespace concurrency
{

#if UINTPTR_MAX == 0xffffffff
template <typename T>
class VersionedPtr
{
public:
    VersionedPtr() : _value(nullptr), _version(0) {}

    VersionedPtr(const T* pointer, uint32_t version)
    {
        _value = const_cast<T*>(pointer);
        _version = version;
    }

    T* operator->() { return _value; }
    const T* operator->() const { return _value; }

    T& operator*() { return *_value; }
    const T& operator*() const { return *_value; }

    uint32_t version() const { return _version; }
    const T* get() const { return _value; }
    T* get() { return _value; }

    bool operator==(const VersionedPtr p) const { return _value == p._value && _version == p._version; }
    explicit operator bool() const { return _value != nullptr; }

private:
    T* _value; // 32-bit pointer
    uint32_t _version;
};
#else

constexpr int VERSION_PTR_SIZE = 48;
constexpr uint64_t VERSION_PTR_MASK = ((1ull << VERSION_PTR_SIZE) - 1) & ~0x7;

// Store version info in upper 16 bits and lower 3 for a total of 19 bits version number.
// It is assumed all VersionedPtr points to objects on 64-bit alignment, thus the 3 LSB must always be 0.
template <typename T>
class VersionedPtr
{
public:
    VersionedPtr() : _value(nullptr) {}

    VersionedPtr(const T* pointer, uint32_t version)
    {
        auto p = reinterpret_cast<uint64_t>(pointer);
        assert((p & 0x7) == 0);
        const uint64_t vLow = version & 0x7;
        const uint64_t vHigh = uint64_t(version & ~0x7) << (VERSION_PTR_SIZE - 3);
        _value = reinterpret_cast<T*>(p | vLow | vHigh);
    }

    T* operator->() { return get(); }
    const T* operator->() const { return get(); }
    T& operator*() { return *get(); }
    const T& operator*() const { return *get(); }

    uint32_t version() const
    {
        auto p = reinterpret_cast<const uint64_t>(_value);
        return (p & 0x7) | (~0x7 & (p >> (VERSION_PTR_SIZE - 3)));
    }

    const T* get() const
    {
        auto p = reinterpret_cast<const uint64_t>(_value);
        return reinterpret_cast<const T*>(p & VERSION_PTR_MASK);
    }

    T* get()
    {
        auto p = reinterpret_cast<uint64_t>(_value);
        return reinterpret_cast<T*>(p & VERSION_PTR_MASK);
    }

    bool operator==(const VersionedPtr p) const { return _value == p._value; }

    explicit operator bool() const { return nullptr != get(); }

private:
    T* _value;
};
#endif
} // namespace concurrency
