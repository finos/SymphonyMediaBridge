#pragma once
#include "utils/FowlerNollHash.h"
#include "utils/StdExtensions.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>

namespace utils
{
/**
 * The purpose of not using std:string is to avoid dynamic memory allocation and also the MpmcHashMap has optimistic
 * reuse. The object may be accessed for reading and it is important the memory has not been deallocated, like in
 * std::string.
 */
template <size_t SIZE>
class FixString
{
public:
    static const size_t capacity = SIZE;

    FixString() : _size(0) { _value[0] = '\0'; }

    explicit FixString(const char* value) : _size(std::min(std::strlen(value), SIZE))
    {
        std::strncpy(_value, value, SIZE);
        _value[SIZE] = '\0';
    }

    explicit FixString(const char* value, size_t length) : _size(std::min(std::strlen(value), std::min(SIZE, length)))
    {
        std::strncpy(_value, value, _size);
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

    int compare(const char* s, size_t length) const
    {
        auto cmp = std::memcmp(_value, s, std::min(_size, length));
        return (cmp == 0 ? (int)(_size - length) : cmp);
    }

    int compare(const char* s) const { return compare(s, std::strlen(s)); }

    __attribute__((format(printf, 1, 2))) static FixString<SIZE> sprintf(const char* format, ...)
    {
        FixString<SIZE> str;

        va_list arglist;
        va_start(arglist, format);
        const int written = std::vsnprintf(str._value, sizeof(str._value), format, arglist);
        va_end(arglist);

        if (written > -1)
        {
            str._size = std::min(static_cast<size_t>(written), SIZE);
        }

        str._value[SIZE] = '\0';
        return str;
    }

private:
    size_t _size;
    char _value[SIZE + 1];
};

template <size_t T>
struct hash<utils::FixString<T>>
{
    uint64_t operator()(const utils::FixString<T>& key) const
    {
        return hash<char*>::hashBuffer(key.c_str(), key.size());
    }
};

} // namespace utils
