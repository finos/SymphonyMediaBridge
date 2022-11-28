#pragma once

#include <cstring>
#include <functional>
#include <string>

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
    return value.size() >= prefix.size() && std::memcmp(prefix.c_str(), value.c_str(), prefix.size()) == 0;
}

/**
 * Alternative implementation to std::hash. This is due to poor support for hashing integers and char* strings. We also
 * want to have same hash value from std::string as from char*
 */
template <typename T>
struct hash : public std::hash<T>
{
};

template <>
struct hash<char*>
{
    static uint64_t hashBuffer(const void* s, size_t len, const uint64_t fnv1Init = 0x84222325cbf29ce4ULL)
    {
        auto cursor = reinterpret_cast<const char*>(s);
        const auto end = cursor + len;

        uint64_t hashValue = fnv1Init;
        while (cursor < end)
        {
            hashValue += (hashValue << 1) + (hashValue << 4) + (hashValue << 5) + (hashValue << 7) + (hashValue << 8) +
                (hashValue << 40);

            hashValue ^= static_cast<uint64_t>(*cursor++);
        }
        return hashValue;
    }

    uint64_t operator()(const char* s) const { return hashBuffer(s, strlen(s)); }
    uint64_t operator()(const char* s, size_t len) const { return hashBuffer(s, len); }
};

template <>
struct hash<const char*>
{
    uint64_t operator()(const char* s) const { return hash<char*>{}(const_cast<char*>(s)); }
};

template <>
struct hash<std::string>
{
    uint64_t operator()(const std::string& s) const { return hash<char*>::hashBuffer(s.c_str(), s.size()); }
};

template <>
struct hash<size_t>
{
    uint64_t operator()(uint64_t key) const { return hash<char*>::hashBuffer(&key, sizeof(key)); }
};

inline bool isNumber(const std::string& str)
{
    return !str.empty() && str.find_first_not_of("0123456789") == std::string::npos;
}

} // namespace utils
