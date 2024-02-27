#include "math/Fields.h"
#include <gtest/gtest.h>

TEST(FieldsTest, videoRewriteRingDifference)
{
    uint16_t sequenceNumber0 = 22787;
    uint16_t sequenceNumber1 = 2112;

    const auto offset1 = math::ringDifference<uint16_t>(sequenceNumber1, sequenceNumber0);
    EXPECT_EQ(20675, offset1);
    EXPECT_EQ(offset1 + sequenceNumber1, sequenceNumber0);

    const auto offset2 = math::ringDifference<uint16_t>(sequenceNumber0, sequenceNumber1);
    EXPECT_EQ(-20675, offset2);
    EXPECT_EQ(offset2 + sequenceNumber0, sequenceNumber1);

    {
        const auto offset = math::ringDifference<uint32_t, 12u>(0xFFF, 444);
        EXPECT_EQ(offset, 445);
        EXPECT_EQ((offset + 0xFFF) & 0xFFF, 444);
    }
    {
        const auto offset = math::ringDifference<uint32_t, 12u>(445, 444);
        EXPECT_EQ(offset, -1);
    }
    {
        const auto offset = math::ringDifference<uint32_t, 12u>(845, 844);
        EXPECT_EQ(offset, -1);
        EXPECT_EQ(offset + 845, 844);
    }

    int32_t diffA = math::ringDifference<uint32_t, 12>(100, 100 + (1 << 10));
    EXPECT_EQ(diffA, int32_t(1 << 10));
    int32_t diffB = math::ringDifference<uint32_t, 12>(100, 100 + (1 << 10) + 1);
    EXPECT_EQ(diffB, (1 << 10) + 1);

    {
        const auto offset = math::ringDifference<uint16_t>(0xFFFF, 444);
        EXPECT_EQ(offset, 445);
    }

    {
        const auto offset = math::ringDifference<uint16_t>(888, 444);
        EXPECT_EQ(offset, -444);
    }
}

TEST(FieldsTest, videoRewriteFullRing)
{
    int32_t pattern[] = {0, 1, 2, 3, -4, -3, -2, -1};
    int32_t offset[8 * 8];

    for (uint32_t i = 0; i < 8; ++i)
    {
        for (uint32_t j = 0; j < 8; ++j)
        {
            offset[i * 8 + j] = math::ringDifference<uint32_t, 3>(i, (j + i) % 8);
        }
    }

    for (int i = 0; i < 64; ++i)
    {
        EXPECT_EQ(offset[i], pattern[i % 8]);
    }
}