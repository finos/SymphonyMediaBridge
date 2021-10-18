#include "utils/OutboundSequenceNumber.h"
#include <gtest/gtest.h>

TEST(OutboundSequenceNumberTest, firstSequenceNumberNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 17;
    uint32_t highestSeenExtendedSequenceNumber = 0xFFFFFFFF;
    uint32_t highestSentExtendedSequenceNumber = 0;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(17, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, firstSequenceNumberWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 17 | (3 << 16); // Sequence number 17 with rollover counter 3
    uint32_t highestSeenExtendedSequenceNumber = 0xFFFFFFFF;
    uint32_t highestSentExtendedSequenceNumber = 0;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(17, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberIsConsecutiveNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 17;
    uint32_t highestSeenExtendedSequenceNumber = 16;
    uint32_t highestSentExtendedSequenceNumber = 16;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(17, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberIsConsecutiveWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 17 | (3 << 16); // Sequence number 17 with rollover counter 3
    uint32_t highestSeenExtendedSequenceNumber = 16 | (3 << 16);
    uint32_t highestSentExtendedSequenceNumber = 16;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(17, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberJumpWithinMarginNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 19;
    uint32_t highestSeenExtendedSequenceNumber = 15;
    uint32_t highestSentExtendedSequenceNumber = 15;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(19, highestSentExtendedSequenceNumber);
    EXPECT_EQ(19, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberJumpWithinMarginWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 10000 | (1 << 16);
    uint32_t highestSeenExtendedSequenceNumber = 9997 | (1 << 16);
    uint32_t highestSentExtendedSequenceNumber = 9997;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(10000, highestSentExtendedSequenceNumber);
    EXPECT_EQ(10000, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberJumpOutsideMarginNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 15 + utils::OutboundSequenceNumber::maxSequenceNumberJump + 1;
    uint32_t highestSeenExtendedSequenceNumber = 15;
    uint32_t highestSentExtendedSequenceNumber = 15;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(extendedSequenceNumber, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(16, highestSentExtendedSequenceNumber);
    EXPECT_EQ(16, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, sequenceNumberJumpOutsideMarginWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 0 | (17 << 16);
    uint32_t highestSeenExtendedSequenceNumber = 10000 | (1 << 16);
    uint32_t highestSentExtendedSequenceNumber = 10000;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(0 | (17 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(10001, highestSentExtendedSequenceNumber);
    EXPECT_EQ(10001, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, oldSequenceNumberWithinMarginNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 15;
    uint32_t highestSeenExtendedSequenceNumber = 17;
    uint32_t highestSentExtendedSequenceNumber = 17;
    uint16_t nextSequenceNumber = 15;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(17, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(15, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, oldSequenceNumberWithinMarginWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 10000 | (1 << 16);
    uint32_t highestSeenExtendedSequenceNumber = 10002 | (1 << 16);
    uint32_t highestSentExtendedSequenceNumber = 10002;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(10002 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(10002, highestSentExtendedSequenceNumber);
    EXPECT_EQ(10000, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, oldSequenceNumberOutsideMarginNoInitialRollover)
{
    const uint32_t extendedSequenceNumber = 0;
    uint32_t highestSeenExtendedSequenceNumber = utils::OutboundSequenceNumber::maxSequenceNumberJump + 1;
    uint32_t highestSentExtendedSequenceNumber = utils::OutboundSequenceNumber::maxSequenceNumberJump + 1;
    uint16_t nextSequenceNumber = 0;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_FALSE(result);
    EXPECT_EQ(utils::OutboundSequenceNumber::maxSequenceNumberJump + 1, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(utils::OutboundSequenceNumber::maxSequenceNumberJump + 1, highestSentExtendedSequenceNumber);
    EXPECT_EQ(0, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, oldSequenceNumberOutsideMarginWithInitialRollover)
{
    const uint32_t extendedSequenceNumber = 17 | (1 << 16) - utils::OutboundSequenceNumber::maxSequenceNumberJump - 1;
    uint32_t highestSeenExtendedSequenceNumber = 17 | (1 << 16);
    uint32_t highestSentExtendedSequenceNumber = 17;
    uint16_t nextSequenceNumber = 15;

    const auto result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_FALSE(result);
    EXPECT_EQ(17 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(17, highestSentExtendedSequenceNumber);
    EXPECT_EQ(15, nextSequenceNumber);
}

TEST(OutboundSequenceNumberTest, testSequence)
{
    uint32_t extendedSequenceNumber;
    uint32_t highestSeenExtendedSequenceNumber;
    uint32_t highestSentExtendedSequenceNumber;
    uint16_t nextSequenceNumber;
    bool result;

    //////

    extendedSequenceNumber = 65535;
    highestSeenExtendedSequenceNumber = 65534;
    highestSentExtendedSequenceNumber = 65534;
    nextSequenceNumber = 0;

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(65535, highestSeenExtendedSequenceNumber);
    EXPECT_EQ(65535, highestSentExtendedSequenceNumber);
    EXPECT_EQ(65535, nextSequenceNumber);

    //////

    extendedSequenceNumber = 0 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(0 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(0 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(0, nextSequenceNumber);

    //////

    extendedSequenceNumber = 3 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(3 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(3 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(3, nextSequenceNumber);

    //////

    extendedSequenceNumber = 2 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(3 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(3 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(2, nextSequenceNumber);

    //////

    extendedSequenceNumber = 4 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(4 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(4 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(4, nextSequenceNumber);

    //////

    extendedSequenceNumber = 20000 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(20000 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(5 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(5, nextSequenceNumber);

    //////

    extendedSequenceNumber = 20001 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(20001 | (1 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(6 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(6, nextSequenceNumber);

    //////

    extendedSequenceNumber = 1 | (1 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_FALSE(result);

    //////

    extendedSequenceNumber = 57 | (3 << 16);

    result = utils::OutboundSequenceNumber::process(extendedSequenceNumber,
        highestSeenExtendedSequenceNumber,
        highestSentExtendedSequenceNumber,
        nextSequenceNumber);

    EXPECT_TRUE(result);
    EXPECT_EQ(57 | (3 << 16), highestSeenExtendedSequenceNumber);
    EXPECT_EQ(7 | (1 << 16), highestSentExtendedSequenceNumber);
    EXPECT_EQ(7, nextSequenceNumber);
}
