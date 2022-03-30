#include "logger/Logger.h"
#include <cstdint>
#include <gtest/gtest.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#else
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <ctime>
#endif

#include "utils/Pacer.h"

class TimeSource
{
    uint64_t timestamp = 0;
#ifdef __APPLE__
    struct mach_timebase_info machTimeBase;
#endif
public:
    TimeSource()
    {
#ifdef __APPLE__
        mach_timebase_info(&machTimeBase);
#endif
    }
    uint64_t getAbsoluteTime() { return timestamp; }

    void incMilliSeconds(int64_t ms)
    {
        if (ms < 0)
        {
            return;
        }
        incNanoSeconds(ms * 1000000);
    }

    void incNanoSeconds(int64_t ns)
    {
        if (ns < 0)
        {
            return;
        }
#ifdef _APPLE
        timestamp += ns * machTimeBase.denom / machTimeBase.numer;
#else
        timestamp += ns;
#endif
    }
};

TEST(Pacer, AutoReset)
{
    const uint64_t INTERVAL = 10 * 1000000;

    TimeSource timeSource;
    logger::info("after sleep", "Test");

    utils::Pacer pacer(INTERVAL);

    int64_t debt = 0;
    int64_t workDurations[] = {6, 7, 11, 12, 15, 15, 15, 15, 15, 15, 15, 7, 8, 4, 4, 4, 4, 4};
    for (int i = 0; i < 19; ++i)
    {
        if (debt > 30)
        {
            debt = 0;
        }
        pacer.tick(timeSource.getAbsoluteTime());
        auto workTime = workDurations[i % (sizeof(workDurations) / sizeof(*workDurations))];
        timeSource.incMilliSeconds(workTime);

        auto toSleep = pacer.timeToNextTick(timeSource.getAbsoluteTime());

        debt += workTime - 10;
        if (debt >= 0)
        {
            EXPECT_LE(toSleep, 0);
        }
        else
        {
            EXPECT_EQ(toSleep, debt * -1000000);
            debt = 0;
        }

        timeSource.incNanoSeconds(toSleep);
    }
}

TEST(Pacer, Regular)
{
    const int64_t INTERVAL = 10;
    utils::Pacer pacer(INTERVAL * 1000000);

    TimeSource timeSource;
    pacer.reset(timeSource.getAbsoluteTime());
    timeSource.incNanoSeconds(pacer.timeToNextTick(timeSource.getAbsoluteTime()));
    for (int i = 0; i < 10; ++i)
    {
        pacer.tick(timeSource.getAbsoluteTime());
        int workTime = 1 + rand() % INTERVAL;
        logger::info("work %d", "Test", workTime);
        timeSource.incMilliSeconds(workTime);

        auto toSleep = pacer.timeToNextTick(timeSource.getAbsoluteTime());
        EXPECT_EQ(1000000 * (10 - workTime), toSleep);

        timeSource.incNanoSeconds(toSleep);
    }
}

TEST(Pacer, ClockWrap)
{
    const int64_t INTERVAL = 10;
    utils::Pacer pacer(INTERVAL * 1000000);

    TimeSource timeSource;
    timeSource.incMilliSeconds(300000);

    for (int i = 0; i < 10; ++i)
    {
        timeSource.incNanoSeconds(pacer.timeToNextTick(timeSource.getAbsoluteTime()));
        pacer.tick(timeSource.getAbsoluteTime());
        EXPECT_EQ(INTERVAL * 1000000ll, pacer.timeToNextTick(timeSource.getAbsoluteTime()));
    }

    // simulate that system is paused and clock wraps to lower value.
    TimeSource timeSource2;
    for (int i = 0; i < 10; ++i)
    {
        pacer.tick(timeSource2.getAbsoluteTime());
        pacer.tick(timeSource2.getAbsoluteTime()); // double tick attempt should not matter
        EXPECT_EQ(INTERVAL * 1000000ll, pacer.timeToNextTick(timeSource2.getAbsoluteTime()));
        timeSource2.incNanoSeconds(pacer.timeToNextTick(timeSource2.getAbsoluteTime()));
    }
}

TEST(Pacer, Hibernate)
{
    const int64_t INTERVAL = 10;
    utils::Pacer pacer(INTERVAL * 1000000);

    TimeSource timeSource;

    for (int i = 0; i < 10; ++i)
    {
        timeSource.incNanoSeconds(pacer.timeToNextTick(timeSource.getAbsoluteTime()));
        pacer.tick(timeSource.getAbsoluteTime());
        EXPECT_EQ(INTERVAL * 1000000ll, pacer.timeToNextTick(timeSource.getAbsoluteTime()));
    }

    // simulate that system is paused 2h
    timeSource.incMilliSeconds(2 * 3600 * 1000);

    for (int i = 0; i < 10; ++i)
    {
        pacer.tick(timeSource.getAbsoluteTime());
        EXPECT_EQ(INTERVAL * 1000000ll, pacer.timeToNextTick(timeSource.getAbsoluteTime()));
        timeSource.incNanoSeconds(pacer.timeToNextTick(timeSource.getAbsoluteTime()));
    }
}
