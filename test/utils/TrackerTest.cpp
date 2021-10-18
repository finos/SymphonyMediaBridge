#include "logger/Logger.h"
#include "utils/Trackers.h"
#include <gtest/gtest.h>
TEST(Trackers, RateTrackerTest)
{
    utils::RateTracker<10> tracker(100 * utils::Time::ms);

    const auto start = utils::Time::getAbsoluteTime();

    for (int i = 0; i < 500; ++i)
    {
        auto now = start + i * 20 * utils::Time::ms;
        tracker.update(180, now);

        if (i > 45)
        {
            EXPECT_NEAR(9000, tracker.get(now, 300 * utils::Time::ms) * utils::Time::sec, 1);
        }
    }
}