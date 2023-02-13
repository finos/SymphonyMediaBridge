#include "codec/H264Header.h"
#include <array>
#include <gtest/gtest.h>

TEST(H264HeaderTest, singleNalUnitNotKeyFrame)
{
    std::array<uint8_t, 2> data = {0x05, 0x00};
    EXPECT_FALSE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, singleNalUnitIsKeyFrame)
{
    std::array<uint8_t, 2> data = {0x07, 0x00};
    EXPECT_TRUE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, fragmenUnitAIsNotKeyFrameNoStartBit)
{
    std::array<uint8_t, 2> data = {0x1C, 0x07};
    EXPECT_FALSE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, fragmenUnitAIsNotKeyFrameNotNalType7)
{
    std::array<uint8_t, 2> data = {0x1C, 0x85};
    EXPECT_FALSE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, fragmenUnitAIsNotKeyFrame)
{
    std::array<uint8_t, 2> data = {0x1C, 0x00};
    EXPECT_FALSE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, fragmenUnitAIsKeyFrame)
{
    std::array<uint8_t, 2> data = {0x1C, 0x87};
    EXPECT_TRUE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, stapAContainsStartOfKeyFrame)
{
    std::array<uint8_t, 5> data = {0x18, 0x01, 0x00, 0x07, 0x00};
    EXPECT_TRUE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, stapAContainsStartOfKeyFrameTwoNalUnits)
{
    std::array<uint8_t, 9> data = {0x18, 0x00, 0x01, 0x05, 0x00, 0x00, 0x01, 0x07, 0x00};
    EXPECT_TRUE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}

TEST(H264HeaderTest, stapAContainsNoStartOfKeyFrame)
{
    std::array<uint8_t, 10> data = {0x18, 0x00, 0x01, 0x05, 0x00, 0x00, 0x02, 0x05, 0x00, 0x00};
    EXPECT_FALSE(codec::H264Header::isKeyFrame(data.data(), data.size()));
}
