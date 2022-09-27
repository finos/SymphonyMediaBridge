#include "bridge/engine/UnackedPacketsTracker.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>

class UnackedPacketsTrackerTest : public ::testing::Test
{
    void SetUp() override
    {
        _unackedPacketsTracker = std::make_unique<bridge::UnackedPacketsTracker>("UnackedPacketsTrackerTest");
    }

    void TearDown() override { _unackedPacketsTracker.reset(); }

protected:
    std::unique_ptr<bridge::UnackedPacketsTracker> _unackedPacketsTracker;
};

TEST_F(UnackedPacketsTrackerTest, test1)
{
    _unackedPacketsTracker->onPacketSent(1, 1 * utils::Time::ms);
    _unackedPacketsTracker->onPacketSent(2, 1 * utils::Time::ms);

    std::array<uint16_t, bridge::UnackedPacketsTracker::maxUnackedPackets> unackedSequenceNumbers{};
    auto numUnackedSequenceNumbers = _unackedPacketsTracker->process(1 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(0, numUnackedSequenceNumbers);

    numUnackedSequenceNumbers = _unackedPacketsTracker->process(1000 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(2, numUnackedSequenceNumbers);
    EXPECT_EQ(1, unackedSequenceNumbers[0]);
    EXPECT_EQ(2, unackedSequenceNumbers[1]);

    numUnackedSequenceNumbers = _unackedPacketsTracker->process(1000 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(0, numUnackedSequenceNumbers);

    numUnackedSequenceNumbers = _unackedPacketsTracker->process(2000 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(2, numUnackedSequenceNumbers);
    EXPECT_EQ(1, unackedSequenceNumbers[0]);
    EXPECT_EQ(2, unackedSequenceNumbers[1]);

    uint32_t extendedSequenceNumber = 0;
    _unackedPacketsTracker->onPacketAcked(2, extendedSequenceNumber);
    EXPECT_EQ(2, extendedSequenceNumber);
    numUnackedSequenceNumbers = _unackedPacketsTracker->process(3000 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(1, numUnackedSequenceNumbers);
    EXPECT_EQ(1, unackedSequenceNumbers[0]);

    for (auto i = 0; i < 10; ++i)
    {
        _unackedPacketsTracker->process((4000 + i * 1000) * utils::Time::ms, unackedSequenceNumbers);
    }

    numUnackedSequenceNumbers = _unackedPacketsTracker->process(1100 * utils::Time::ms, unackedSequenceNumbers);
    EXPECT_EQ(0, numUnackedSequenceNumbers);
}
