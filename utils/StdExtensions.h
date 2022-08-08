#pragma once
#include <string>
#include <cstring>

namespace std
{
#if __cplusplus < 201703L
template <class T, std::size_t N>
constexpr std::size_t size(const T (&array)[N])
{
    return N;
}
#endif

template <typename T>
T max(const T& a, const T& b, const T& c)
{
    return std::max(a, std::max(b, c));
}

template <typename T>
T max(const T& a, const T& b, const T& c, const T& d)
{
    return std::max(a, std::max(b, std::max(c, d)));
}
} // namespace std

namespace utils
{
// return true if string fit into destination with null-term
// truncates and ensures null termination
inline bool strncpy(char* dst, const char* src, size_t maxLength)
{
    // will pad with null until maxLength
    std::strncpy(dst, src, maxLength);

    if (dst[maxLength - 1] == '\0')
    {
        return true;
    }
    dst[maxLength - 1] = '\0';
    return false;
}

inline bool startsWith(const std::string& prefix, const std::string& value)
{
    return value.size() >= prefix.size() &&
        std::memcmp(prefix.c_str(), value.c_str(), prefix.size()) == 0;
}

} // namespace utils
