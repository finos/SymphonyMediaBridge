#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/StackArray.h"
#include <array>

namespace bridge
{

/**
 * The purpose of not using std:string is to avoid dynamic memory allocation and also the map has optimistic reuse. The
 * object may be accessed for reading and it is important the memory has not been deallocated, like in std::string.
 */
template <size_t SIZE>
class FixString
{
public:
    FixString() : _size(0) { _value[0] = '\0'; }

    explicit FixString(const char* value) : _size(std::min(std::strlen(value), SIZE))
    {
        std::strncpy(_value, value, SIZE);
        _value[SIZE] = '\0';
    }

    FixString& operator=(const char* value)
    {
        _size = std::min(std::strlen(value), SIZE);
        std::strncpy(_value, value, SIZE);
        _value[SIZE] = '\0';
        return *this;
    }

    const char* c_str() const { return _value; }

    size_t size() const { return _size; }

private:
    size_t _size;
    char _value[SIZE + 1];
};

using EndpointIdString = FixString<42>;
using BarbellEndpointIdMap = concurrency::MpmcHashmap32<size_t, EndpointIdString>;

struct BarbellMapItem
{
    EndpointIdString endpointId;
    size_t endpointIdHash = 0;
    std::array<uint32_t, 2> ssrcs;
    uint32_t count = 0;
};

} // namespace bridge
