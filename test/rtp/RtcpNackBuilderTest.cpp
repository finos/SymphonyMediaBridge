#include "rtp/RtcpNackBuilder.h"
#include "rtp/RtcpFeedback.h"
#include <gtest/gtest.h>

TEST(RtcpNackBuilderTest, firstAppendedPidShouldBeInFirstPid)
{
    const auto reporterSsrc = 1;
    const auto mediaSsrc = 2;

    rtp::RtcpNackBuilder rtcpNackBuilder(reporterSsrc, mediaSsrc);
    const auto appendResult = rtcpNackBuilder.appendSequenceNumber(1234);
    EXPECT_TRUE(appendResult);

    size_t rtcpNackSize = 0;
    const auto rtcpNack = rtcpNackBuilder.build(rtcpNackSize);

    EXPECT_EQ(sizeof(rtp::RtcpFeedback) + 4, rtcpNackSize);

    auto rtcpFeedbackHeader = reinterpret_cast<const rtp::RtcpFeedback*>(rtcpNack);
    EXPECT_EQ(1, rtcpFeedbackHeader->_reporterSsrc);
    EXPECT_EQ(2, rtcpFeedbackHeader->_mediaSsrc);
    EXPECT_EQ(3, rtcpFeedbackHeader->_header.length.get());

    EXPECT_EQ(0x04, rtcpNack[12]);
    EXPECT_EQ(0xD2, rtcpNack[13]);
    EXPECT_EQ(0x00, rtcpNack[14]);
    EXPECT_EQ(0x00, rtcpNack[15]);
}

TEST(RtcpNackBuilderTest, additionalAdjacentAppendedPidsShouldBeIncludedInBlp)
{
    const auto reporterSsrc = 1;
    const auto mediaSsrc = 2;

    rtp::RtcpNackBuilder rtcpNackBuilder(reporterSsrc, mediaSsrc);
    auto appendResult = rtcpNackBuilder.appendSequenceNumber(1234);
    EXPECT_TRUE(appendResult);

    appendResult = rtcpNackBuilder.appendSequenceNumber(1235);
    EXPECT_TRUE(appendResult);

    appendResult = rtcpNackBuilder.appendSequenceNumber(1250);
    EXPECT_TRUE(appendResult);

    size_t rtcpNackSize = 0;
    const auto rtcpNack = rtcpNackBuilder.build(rtcpNackSize);

    EXPECT_EQ(sizeof(rtp::RtcpFeedback) + 4, rtcpNackSize);

    auto rtcpFeedbackHeader = reinterpret_cast<const rtp::RtcpFeedback*>(rtcpNack);
    EXPECT_EQ(1, rtcpFeedbackHeader->_reporterSsrc);
    EXPECT_EQ(2, rtcpFeedbackHeader->_mediaSsrc);
    EXPECT_EQ(3, rtcpFeedbackHeader->_header.length.get());

    EXPECT_EQ(0x04, rtcpNack[12]);
    EXPECT_EQ(0xD2, rtcpNack[13]);
    EXPECT_EQ(0x80, rtcpNack[14]);
    EXPECT_EQ(0x01, rtcpNack[15]);
}

TEST(RtcpNackBuilderTest, additionalNonAdjacentAppendedPidsShouldBeInNewPid)
{
    const auto reporterSsrc = 1;
    const auto mediaSsrc = 2;

    rtp::RtcpNackBuilder rtcpNackBuilder(reporterSsrc, mediaSsrc);
    auto appendResult = rtcpNackBuilder.appendSequenceNumber(1234);
    EXPECT_TRUE(appendResult);

    appendResult = rtcpNackBuilder.appendSequenceNumber(1251);
    EXPECT_TRUE(appendResult);

    size_t rtcpNackSize = 0;
    const auto rtcpNack = rtcpNackBuilder.build(rtcpNackSize);

    EXPECT_EQ(sizeof(rtp::RtcpFeedback) + 8, rtcpNackSize);

    auto rtcpFeedbackHeader = reinterpret_cast<const rtp::RtcpFeedback*>(rtcpNack);
    EXPECT_EQ(1, rtcpFeedbackHeader->_reporterSsrc);
    EXPECT_EQ(2, rtcpFeedbackHeader->_mediaSsrc);
    EXPECT_EQ(4, rtcpFeedbackHeader->_header.length.get());

    EXPECT_EQ(0x04, rtcpNack[12]);
    EXPECT_EQ(0xD2, rtcpNack[13]);
    EXPECT_EQ(0x00, rtcpNack[14]);
    EXPECT_EQ(0x00, rtcpNack[15]);
    EXPECT_EQ(0x04, rtcpNack[16]);
    EXPECT_EQ(0xE3, rtcpNack[17]);
    EXPECT_EQ(0x00, rtcpNack[18]);
    EXPECT_EQ(0x00, rtcpNack[19]);
}

TEST(RtcpNackBuilderTest, appendWhenPidsArrayIsFullReturnsFalse)
{
    const auto reporterSsrc = 1;
    const auto mediaSsrc = 2;

    rtp::RtcpNackBuilder rtcpNackBuilder(reporterSsrc, mediaSsrc);

    for (size_t i = 0; i < rtp::RtcpNackBuilder::maxPids; ++i)
    {
        const auto appendResult = rtcpNackBuilder.appendSequenceNumber(i * 17);
        EXPECT_TRUE(appendResult);
    }

    const auto appendResult = rtcpNackBuilder.appendSequenceNumber(300);
    EXPECT_FALSE(appendResult);
}
