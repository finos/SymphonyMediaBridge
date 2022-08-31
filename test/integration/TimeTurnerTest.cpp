#include "emulator/TimeTurner.h"
#include "logger/Logger.h"
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>

TEST(TimeTurner, threads3)
{
    emulator::TimeTurner timeSource;
    utils::Time::initialize(timeSource);

    std::atomic_bool terminate(false);
    uint32_t count[3] = {0};
    std::thread thread1([&terminate, &count] {
        while (!terminate)
        {
            ++count[0];
            utils::Time::nanoSleep(100 * utils::Time::ms);
            logger::info("slept %u", "thread1", count[0]);
        }
    });

    std::thread thread2([&terminate, &count] {
        while (!terminate)
        {
            ++count[1];
            utils::Time::nanoSleep(125 * utils::Time::ms);
            logger::info("slept %u", "thread2", count[1]);
        }
    });

    std::thread thread3([&terminate, &count] {
        while (!terminate)
        {
            ++count[2];
            utils::Time::nanoSleep(250 * utils::Time::ms);
            logger::info("slept %u", "thread3", count[2]);
        }
    });

    timeSource.waitForThreadsToSleep(3, 3000);

    timeSource.runFor(utils::Time::ms * 999);
    logger::info("finished waiting", "main");
    terminate = true;
    timeSource.shutdown();
    thread1.join();
    thread2.join();
    thread3.join();
    EXPECT_EQ(count[0], 11);
    EXPECT_EQ(count[1], 9);
    EXPECT_EQ(count[2], 5);
}
