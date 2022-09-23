#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "utils/Time.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>

class VideoMissingPacketsTrackerTest : public ::testing::Test
{
    void SetUp() override { _videoMissingPacketsTracker = std::make_unique<bridge::VideoMissingPacketsTracker>(0); }

    void TearDown() override { _videoMissingPacketsTracker.reset(); }

protected:
    std::unique_ptr<bridge::VideoMissingPacketsTracker> _videoMissingPacketsTracker;
};

TEST_F(VideoMissingPacketsTrackerTest, test1)
{
    _videoMissingPacketsTracker->onMissingPacket(1, 1 * utils::Time::ms);
    _videoMissingPacketsTracker->onMissingPacket(2, 1 * utils::Time::ms);

    std::array<uint16_t, bridge::VideoMissingPacketsTracker::maxMissingPackets> missingSequenceNumbers;
    auto numMissingSequenceNumbers = _videoMissingPacketsTracker->process(0, 100, missingSequenceNumbers);
    EXPECT_EQ(0, numMissingSequenceNumbers);

    numMissingSequenceNumbers =
        _videoMissingPacketsTracker->process(1000 * utils::Time::ms, 100, missingSequenceNumbers);
    EXPECT_EQ(2, numMissingSequenceNumbers);
    EXPECT_EQ(1, missingSequenceNumbers[0]);
    EXPECT_EQ(2, missingSequenceNumbers[1]);

    numMissingSequenceNumbers =
        _videoMissingPacketsTracker->process(1000 * utils::Time::ms, 100, missingSequenceNumbers);
    EXPECT_EQ(0, numMissingSequenceNumbers);

    numMissingSequenceNumbers =
        _videoMissingPacketsTracker->process(2000 * utils::Time::ms, 100, missingSequenceNumbers);
    EXPECT_EQ(2, numMissingSequenceNumbers);
    EXPECT_EQ(1, missingSequenceNumbers[0]);
    EXPECT_EQ(2, missingSequenceNumbers[1]);

    uint32_t extendedSequenceNumber = 0;
    _videoMissingPacketsTracker->onPacketArrived(2, extendedSequenceNumber);
    EXPECT_EQ(2, extendedSequenceNumber);
    numMissingSequenceNumbers =
        _videoMissingPacketsTracker->process(3000 * utils::Time::ms, 100, missingSequenceNumbers);
    EXPECT_EQ(1, numMissingSequenceNumbers);
    EXPECT_EQ(1, missingSequenceNumbers[0]);

    for (auto i = 0; i < 10; ++i)
    {
        _videoMissingPacketsTracker->process((4000 + i * 1000) * utils::Time::ms, 100, missingSequenceNumbers);
    }

    numMissingSequenceNumbers =
        _videoMissingPacketsTracker->process(1100 * utils::Time::ms, 100, missingSequenceNumbers);
    EXPECT_EQ(0, numMissingSequenceNumbers);
}
