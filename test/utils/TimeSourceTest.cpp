#include "logger/Logger.h"
#include "utils/Time.h"
#include <chrono>
#include <gtest/gtest.h>
TEST(DISABLED_TimeSource, Comparison)
{
    auto startAbs = utils::Time::getAbsoluteTime();
    auto startNtp = std::chrono::system_clock::now();
    auto startSteady = std::chrono::steady_clock::now();
    utils::Time::nanoSleep(30 * utils::Time::sec);
    auto endAbs = utils::Time::getAbsoluteTime();
    auto endNtp = std::chrono::system_clock::now();
    auto endSteady = std::chrono::steady_clock::now();

    logger::info("abs diff %" PRIu64, "", (endAbs - startAbs) / utils::Time::us);
    logger::info("ntp diff %llu", "", std::chrono::duration_cast<std::chrono::microseconds>(endNtp - startNtp).count());
    logger::info("steady diff %llu",
        "",
        std::chrono::duration_cast<std::chrono::microseconds>(endSteady - startSteady).count());
}