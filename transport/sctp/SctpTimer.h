#pragma once
#include <cstdint>

namespace sctp
{
namespace timer
{
const uint64_t us = 1000;
const uint64_t ms = 1000 * us;
const uint64_t sec = ms * 1000;
}; // namespace timer

class Timer
{
public:
    Timer(uint32_t timeoutMs, uint32_t maxTimeoutMs) : _expires(0), _maxTimeout(maxTimeoutMs * timer::ms), _armed(false)
    {
    }

    void startMs(uint64_t timestamp, uint32_t timeoutMs);
    void startNs(uint64_t timestamp, uint64_t timeoutNs);
    void expireAt(uint64_t timestamp);
    bool isRunning() const { return _armed; }
    bool hasExpired(uint64_t timestamp) const;

    int64_t remainingTime(uint64_t timestamp) const;

    void stop();

private:
    uint64_t _expires;
    uint64_t _maxTimeout;
    bool _armed;
};
} // namespace sctp