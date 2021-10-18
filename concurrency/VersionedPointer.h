#pragma once
#include <cassert>
#include <inttypes.h>
namespace concurrency
{

constexpr int VERSION_PTR_SIZE = 48;
constexpr uint64_t VERSION_PTR_MASK = ((1ull << VERSION_PTR_SIZE) - 1) & ~0x7;
constexpr uint32_t MAX_NODE_VERSION = (1 << (61 - VERSION_PTR_SIZE)) - 1;
// 19 bit version is split in two parts. Upper 16 bits and lower 3 bits.
// Stored above 48 bit and in 3 LSB
// This means all pointer values have to be 8-byte aligned
template <typename T>
T* makeVersionedPointer(T* pointer, uint32_t version)
{
    auto p = reinterpret_cast<uint64_t>(pointer);
    assert((p & 0x7) == 0);
    uint64_t vLow = version & 0x7;
    uint64_t vHigh = version & ~0x7;
    vHigh <<= (VERSION_PTR_SIZE - 3);
    return reinterpret_cast<T*>(p | vLow | vHigh);
}

template <typename T>
uint32_t getVersion(const T* pointer)
{
    uint64_t p = reinterpret_cast<uint64_t>(pointer);
    return (p & 0x7) | (~0x7 & (p >> (VERSION_PTR_SIZE - 3)));
}

template <typename T>
T* getPointer(const T* versionedPointer)
{
    uint64_t p = reinterpret_cast<uint64_t>(versionedPointer);
    return reinterpret_cast<T*>(p & VERSION_PTR_MASK);
}

}