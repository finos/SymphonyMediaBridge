#pragma once

#include "logger/Logger.h"
#include <array>
#include <string>

namespace utils
{

template <size_t S>
class StringBuilder
{
public:
    StringBuilder() : _offset(0) { std::memset(_data, 0, S); }

    StringBuilder& append(const std::string& string)
    {
        if (!checkLength(string.length()))
        {
            return *this;
        }

        auto data = &_data[_offset];
        auto remainingBytes = S - _offset;

        std::strncpy(data, string.c_str(), remainingBytes);
        _offset += string.length();
        return *this;
    }

    StringBuilder& append(const char* string)
    {
        auto remainingBytes = S - _offset;
        const auto length = strnlen(string, S - _offset);

        if (!checkLength(length))
        {
            return *this;
        }

        auto data = &_data[_offset];
        std::strncpy(data, string, remainingBytes);
        _offset += strnlen(string, remainingBytes);
        return *this;
    }

    StringBuilder& append(const char* string, const size_t length)
    {
        if (!checkLength(length))
        {
            return *this;
        }

        auto data = &_data[_offset];
        auto remainingBytes = S - _offset;

        std::strncpy(data, string, remainingBytes);
        data[length] = '\0';
        _offset += length;
        return *this;
    }

    StringBuilder& append(const uint32_t value)
    {
        char valueString[16];
        std::snprintf(valueString, 16, "%u", value);

        auto remainingBytes = S - _offset;
        const auto length = strnlen(valueString, S - _offset);

        if (!checkLength(length))
        {
            return *this;
        }

        auto data = &_data[_offset];
        std::strncpy(data, valueString, remainingBytes);
        _offset += strnlen(valueString, remainingBytes);
        return *this;
    }

    std::string build() const { return std::string(_data); }

    const char* get() const { return _data; }
    const size_t getLength() const { return _offset; };

    void clear()
    {
        _offset = 0;
        std::memset(_data, 0, S);
    }

    bool endsWidth(char c) const { return _offset > 0 && _data[_offset - 1] == c; }
    bool empty() const { return _offset == 0; }

private:
    char _data[S];
    size_t _offset;

    bool checkLength(const size_t length)
    {
        const bool result = _offset + length + 1 <= S;
        if (!result)
        {
            logger::warn("No space left, size %lu, remaining size %lu, length %lu",
                "StringBuilder",
                S,
                (S - _offset - 1),
                length);
        }
        return result;
    }
};

} // namespace utils
