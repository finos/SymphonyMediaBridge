#pragma once

#include <chrono>
#include <cstdint>

namespace utils
{

namespace Time
{

void initialize();

/**
 * Returns absolute time since some machine specific point in time in nanoseconds.
 */
uint64_t getAbsoluteTime();
uint64_t getApproximateTime();
void nanoSleep(int64_t ns);

void usleep(long uSec);

uint64_t toNtp(std::chrono::system_clock::time_point timestamp);
inline uint32_t toNtp32(const std::chrono::system_clock::time_point timestamp)
{
    return 0xFFFFFFFFu & (utils::Time::toNtp(timestamp) >> 16);
}

inline uint32_t toNtp32(const uint64_t ntpTimestamp)
{
    return 0xFFFFFFFFu & (ntpTimestamp >> 16);
}

constexpr uint64_t us = 1000;
constexpr uint64_t ms = 1000 * us;
constexpr uint64_t sec = ms * 1000;
constexpr uint64_t minute = sec * 60;

inline uint32_t absToNtp32(const uint64_t timestamp)
{
    return 0xFFFFFFFFu & (timestamp * 0x10000u / utils::Time::sec);
}

constexpr int64_t diff(const uint64_t a, const uint64_t b)
{
    return static_cast<int64_t>(b - a);
}

constexpr bool diffLT(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return static_cast<int64_t>(b - a) < static_cast<int64_t>(value);
}

constexpr bool diffLE(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return static_cast<int64_t>(b - a) <= static_cast<int64_t>(value);
}

constexpr bool diffGT(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return static_cast<int64_t>(b - a) > static_cast<int64_t>(value);
}

constexpr bool diffGE(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return static_cast<int64_t>(b - a) >= static_cast<int64_t>(value);
}

} // namespace Time

} // namespace utils
