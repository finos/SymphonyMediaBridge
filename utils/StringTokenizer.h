#pragma once

#include <cassert>
#include <cstring>
#include <stddef.h>
#include <string>

namespace utils
{

namespace StringTokenizer
{

struct Token
{
    const char* start;
    size_t length;
    const char* next;
    size_t remainingLength;

    bool empty() const { return start == nullptr; }
    std::string str() const { return std::string(start, length); }
};

inline Token tokenize(const char* data, const size_t length, const char delimiter)
{
    assert(data);
    if (length == 0)
    {
        return Token({nullptr, 0, nullptr, 0});
    }

    const char* start = data;
    size_t remainingLength = length;

    for (size_t i = 0; i < length; ++i)
    {
        if (data[i] != delimiter)
        {
            break;
        }

        if (i == length - 1)
        {
            return Token({nullptr, 0, nullptr, 0});
        }
        else
        {
            start = &(data[i + 1]);
            --remainingLength;
        }
    }

    for (size_t i = 0; i < remainingLength; ++i)
    {
        if (start[i] != delimiter)
        {
            continue;
        }

        if (i == remainingLength - 1)
        {
            return Token({start, i, nullptr, 0});
        }
        else
        {
            return Token({start, i, &(start[i + 1]), remainingLength - i - 1});
        }
    }

    return Token({start, remainingLength, nullptr, 0});
}

inline Token tokenize(const Token& token, const char delimiter)
{
    if (token.next == nullptr)
    {
        return Token({nullptr, 0, nullptr, 0});
    }
    return tokenize(token.next, token.remainingLength, delimiter);
}

inline bool isEqual(const Token& token, const Token& other)
{
    if (token.length == 0 && other.length == 0)
    {
        return true;
    }
    else if (token.length != other.length)
    {
        return false;
    }

    return strncmp(token.start, other.start, token.length) == 0;
}

inline bool isEqual(const Token& token, const char* other, const size_t otherLength)
{
    if (token.length == 0 && otherLength == 0)
    {
        return true;
    }
    else if (token.length != otherLength)
    {
        return false;
    }

    return strncmp(token.start, other, token.length) == 0;
}

inline bool isEqual(const Token& token, const char* other)
{
    const auto otherLength = strlen(other);

    if (token.length == 0 && otherLength == 0)
    {
        return true;
    }
    else if (token.length != otherLength)
    {
        return false;
    }

    return strncmp(token.start, other, token.length) == 0;
}

} // namespace StringTokenizer

} // namespace utils
