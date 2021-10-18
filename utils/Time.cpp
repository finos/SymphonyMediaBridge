#include <cassert>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#else
#include <ctime>
#include <pthread.h>

#endif
#include "utils/Time.h"
#include <thread>
#include <time.h>
#include <unistd.h>

namespace
{
#ifdef __APPLE__
struct mach_timebase_info machTimeBase;
#endif
}

namespace utils
{

namespace Time
{

void initialize()
{
#ifdef __APPLE__
    mach_timebase_info(&machTimeBase);
#endif
}

uint64_t getAbsoluteTime()
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

// faster on Mac
uint64_t getApproximateTime()
{
#ifdef __APPLE__
    assert(machTimeBase.denom);
    return mach_continuous_approximate_time() * machTimeBase.numer / machTimeBase.denom;
#else
    return getAbsoluteTime();
#endif
}

void nanoSleep(int64_t ns)
{
    ns = (ns > 0 ? ns : 0);
    const timespec sleepTime = {static_cast<long>(ns / 1000000000UL), static_cast<long>(ns % 1000000000UL)};
    ::nanosleep(&sleepTime, nullptr);
}

void usleep(long uSec) { nanoSleep(static_cast<uint64_t>(uSec) * 1000); }

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

}

}
