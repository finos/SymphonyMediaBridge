#pragma once
#include <chrono>
#include <cstdint>
#include <memory>

namespace utils
{

// Override this and provide implementation in initialize.
// Call cleanup if you passed ownership of the object to Time
class TimeSource
{
public:
    virtual ~TimeSource() = default;

    virtual uint64_t getAbsoluteTime() const = 0;
    virtual uint64_t getApproximateTime() const { return getAbsoluteTime(); };

    virtual void nanoSleep(uint64_t nanoSeconds) = 0;

    virtual std::chrono::system_clock::time_point wallClock() const = 0;
    virtual void advance(uint64_t nanoSeconds) = 0;
};

namespace Time
{

void initialize();
void initialize(TimeSource& timeSource);
bool isDefaultTimeSource();

/**
 * Returns absolute time since some machine specific point in time in nanoseconds.
 */
uint64_t getAbsoluteTime();
uint64_t getRawAbsoluteTime();
uint64_t getApproximateTime();
void nanoSleep(int64_t ns);
void nanoSleep(int32_t ns);
void nanoSleep(uint64_t ns);
void nanoSleep(uint32_t ns);

void uSleep(int64_t uSec);
void mSleep(int64_t milliSeconds);

std::chrono::system_clock::time_point now();

void rawNanoSleep(int64_t ns);
uint64_t rawAbsoluteTime();

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
    return diff(a, b) < static_cast<int64_t>(value);
}

constexpr bool diffLE(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return diff(a, b) <= static_cast<int64_t>(value);
}

constexpr bool diffGT(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return diff(a, b) > static_cast<int64_t>(value);
}

constexpr bool diffGE(const uint64_t a, const uint64_t b, const uint64_t value)
{
    return diff(a, b) >= static_cast<int64_t>(value);
}
} // namespace Time

} // namespace utils
