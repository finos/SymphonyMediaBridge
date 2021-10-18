
#include "rtp/SendTimeDial.h"
#include "utils/Time.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>

uint32_t createSendTime(uint64_t timestamp)
{
    const uint64_t GCD = 512; // to minimize shift out
    const uint64_t NOMINATOR = (1 << 18) / GCD;
    const uint64_t DENOMINATOR = utils::Time::sec / GCD;
    return ((timestamp * NOMINATOR) / DENOMINATOR) & 0xFFFFFFu;
}

TEST(RtpSendTimeTest, wrap)
{
    uint64_t start = 982348934;
    uint64_t sendTime;
    rtp::SendTimeDial converter;
    auto now = start + utils::Time::sec * 5;
    for (int i = 0; i < 25000; ++i)
    {
        now += utils::Time::ms * 10;
        sendTime = createSendTime(start + i * utils::Time::ms * 10);
        auto sendTimestamp = converter.toAbsoluteTime(sendTime, now);
        EXPECT_LE(now - sendTimestamp, utils::Time::sec * 15 + utils::Time::ms * 11);
    }
}

TEST(RtpSendTimeTest, reordering)
{
    uint64_t localTimer = 979886;
    uint64_t remoteTimer = 2342334;

    rtp::SendTimeDial converter;
    const auto localTick = utils::Time::sec / 512;
    const auto remoteTick = (1 << 18) / 512;

    auto sendTimestamp = converter.toAbsoluteTime(remoteTimer % (1 << 24), localTimer);
    int64_t offset = localTimer - sendTimestamp;

    for (int i = 0; i < 25000; ++i)
    {
        auto x = rand() % 25;
        localTimer += localTick * x;
        remoteTimer += remoteTick * x;
        auto delta = rand() % 2000;
        auto sendTime = remoteTimer + delta;
        auto sendTimestamp = converter.toAbsoluteTime(sendTime % (1 << 24), localTimer);

        EXPECT_NEAR(localTimer - offset + delta * 3815, sendTimestamp, 3500);
    }
}

TEST(RtpSendTimeTest, corner)
{
    rtp::SendTimeDial converter;
    auto sendTimestamp = converter.toAbsoluteTime(0, 0);
    EXPECT_EQ(sendTimestamp, uint64_t(0) - uint64_t(2 * 1953125));

    sendTimestamp = converter.toAbsoluteTime((1 << 24) - 5, utils::Time::ms * 50);
    EXPECT_EQ(sendTimestamp, uint64_t(0) - uint64_t(2 * 1953125) - 5 * 3815 + 2);

    sendTimestamp = converter.toAbsoluteTime(512 * 2, utils::Time::ms * 51);
    EXPECT_EQ(sendTimestamp, uint64_t(0));
}