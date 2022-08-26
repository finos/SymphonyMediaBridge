#include "utils/Time.h"
#include <algorithm>
#include <cassert>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#else
#include <ctime>
#include <pthread.h>
#endif
namespace
{
#ifdef __APPLE__
struct mach_timebase_info machTimeBase;
#endif
} // namespace

namespace utils
{

namespace Time
{
// global time source
utils::TimeSource* _timeSource = nullptr;

class TimeSourceImpl final : public utils::TimeSource
{
public:
    uint64_t getAbsoluteTime() override
    {
#ifdef __APPLE__
        assert(machTimeBase.denom);
        return mach_absolute_time() * machTimeBase.numer / machTimeBase.denom;
#else
        timespec timeSpec = {};
        clock_gettime(CLOCK_MONOTONIC, &timeSpec);
        return static_cast<uint64_t>(timeSpec.tv_sec) * 1000000000ULL + static_cast<uint64_t>(timeSpec.tv_nsec);
#endif
    }

    uint64_t getApproximateTime() override
    {
#ifdef __APPLE__
        assert(machTimeBase.denom);
        return mach_continuous_approximate_time() * machTimeBase.numer / machTimeBase.denom;
#else
        return getAbsoluteTime();
#endif
    }

    void nanoSleep(uint64_t ns) override
    {
        ns = std::min(ns, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) * uint64_t(1000'000'000));
        const timespec sleepTime = {static_cast<long>(ns / 1000'000'000UL), static_cast<long>(ns % 1000000000UL)};
        ::nanosleep(&sleepTime, nullptr);
    }

    void nanoSleep(int64_t nanoSeconds) override
    {
        uint64_t ns = (nanoSeconds > 0 ? nanoSeconds : 0);
        nanoSleep(ns);
    }
    void uSleep(uint64_t uSec) override { utils::Time::nanoSleep(uSec * utils::Time::us); }
    void mSleep(uint64_t milliSeconds) override { utils::Time::nanoSleep(milliSeconds * utils::Time::ms); }

    std::chrono::system_clock::time_point wallClock() override { return std::chrono::system_clock::now(); }
};

void initialize()
{
#ifdef __APPLE__
    mach_timebase_info(&machTimeBase);
#endif
    _timeSource = new TimeSourceImpl();
}

void initialize(TimeSource* timeSource)
{
    if (!_timeSource)
    {
#ifdef __APPLE__
        mach_timebase_info(&machTimeBase);
#endif
    }
    else
    {
        delete _timeSource;
    }

    _timeSource = timeSource;
}

void cleanup()
{
    delete _timeSource;
}

uint64_t getAbsoluteTime()
{
    return _timeSource->getAbsoluteTime();
}

// faster on Mac
uint64_t getApproximateTime()
{
    return _timeSource->getApproximateTime();
}

void nanoSleep(int64_t ns)
{
    _timeSource->nanoSleep(ns);
}

void usleep(long uSec)
{
    _timeSource->nanoSleep(static_cast<uint64_t>(uSec) * 1000);
}

uint64_t toNtp(const std::chrono::system_clock::time_point timestamp)
{
    uint64_t ntp = 0;
    using namespace std::chrono;
    const auto durationSinceEpoch = timestamp.time_since_epoch();
    const std::chrono::seconds offset70y((70 * 365 + 17) * 86400ll);
    const auto since1900s(durationSinceEpoch + offset70y);
    ntp = duration_cast<std::chrono::seconds>(since1900s).count();
    ntp <<= 32;
    const uint64_t multiplier = 0x100000000 / 64; // 64 is GCF
    const uint64_t divisor = 1000000 / 64;
    ntp |= duration_cast<microseconds>(durationSinceEpoch % std::chrono::seconds(1)).count() * multiplier / divisor;
    return ntp;
}

} // namespace Time

} // namespace utils
