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

TEST(Trackers, AvgRateTrackerTest)
{
    utils::AvgRateTracker tracker(0.1);

    const auto start = utils::Time::getAbsoluteTime();

    for (int i = 0; i < 500; ++i)
    {
        auto now = start + (i * 20 - 3 + rand() % 7) * utils::Time::ms;
        tracker.update(175 + rand() % 11, now);

        if (i > 45)
        {
            EXPECT_NEAR(9000.0, tracker.get(), 250.0);
        }
    }
}

TEST(Trackers, AvgPpsTrackerTest)
{
    utils::AvgRateTracker tracker(0.1);

    const auto start = utils::Time::getAbsoluteTime();
    tracker.update(1, start);
    tracker.update(1, start);
    for (int i = 1; i < 500; ++i)
    {
        auto now = start + (i * 20 - 3 + rand() % 7) * utils::Time::ms;
        tracker.update(1, now);

        if (i > 45)
        {
            EXPECT_NEAR(50.0, tracker.get(), 1.1);
        }
    }
}
