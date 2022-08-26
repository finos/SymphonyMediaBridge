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

std::chrono::system_clock::time_point now()
{
    return _timeSource->wallClock();
}

void nanoSleep(int64_t ns)
{
    _timeSource->nanoSleep(ns > 0 ? ns : 0);
}

void nanoSleep(uint64_t ns)
{
    _timeSource->nanoSleep(ns);
}

void nanoSleep(int32_t ns)
{
    _timeSource->nanoSleep(ns > 0 ? ns : 0);
}

void nanoSleep(uint32_t ns)
{
    _timeSource->nanoSleep(ns);
}

void uSleep(int64_t uSec)
{
    _timeSource->nanoSleep((uSec > 0 ? uSec : 0) * 1000);
}

void mSleep(int64_t milliSeconds)
{
    _timeSource->nanoSleep((milliSeconds > 0 ? milliSeconds : 0) * 1000000);
}

// OS sleep bypass TimeSource
void rawNanoSleep(int64_t ns)
{
    ns = (ns > 0 ? ns : 0);
    ns = std::min(ns, static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * int64_t(1000'000'000));
    const timespec sleepTime = {static_cast<long>(ns / 1000'000'000UL), static_cast<long>(ns % 1000000000UL)};
    ::nanosleep(&sleepTime, nullptr);
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
