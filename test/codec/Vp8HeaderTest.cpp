#include "codec/Vp8Header.h"
#include <array>
#include <gtest/gtest.h>

TEST(Vp8HeaderTest, notKeyFrame0)
{
    std::array<uint8_t, 7> data = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60, 0x11};

    const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(data.data(), data.size());
    EXPECT_EQ(6, payloadDescriptorSize);
    EXPECT_FALSE(codec::Vp8Header::isKeyFrame(data.data(), payloadDescriptorSize));
}

TEST(Vp8HeaderTest, notKeyFrame1)
{
    std::array<uint8_t, 7> data = {0xb0, 0xe0, 0xbd, 0x9d, 0x3e, 0x40, 0xb1};

    const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(data.data(), data.size());
    EXPECT_EQ(6, payloadDescriptorSize);
    EXPECT_FALSE(codec::Vp8Header::isKeyFrame(data.data(), payloadDescriptorSize));
}

TEST(Vp8HeaderTest, KeyFrameFirst)
{
    std::array<uint8_t, 7> data = {0xb0, 0xe0, 0xbd, 0x9d, 0x3e, 0x40, 0xb0};

    const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(data.data(), data.size());
    EXPECT_EQ(6, payloadDescriptorSize);
    EXPECT_EQ(codec::Vp8Header::getPicId(data.data()), 0x3d9d);
    EXPECT_TRUE(codec::Vp8Header::isKeyFrame(data.data(), payloadDescriptorSize));
}

TEST(Vp8HeaderTest, KeyFrameLast)
{
    std::array<uint8_t, 7> data = {0xa0, 0xe0, 0xbd, 0x9d, 0x3e, 0x40, 0xb0};

    const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(data.data(), data.size());
    EXPECT_EQ(6, payloadDescriptorSize);
    EXPECT_EQ(codec::Vp8Header::getPicId(data.data()), 0x3d9d);
    EXPECT_FALSE(codec::Vp8Header::isKeyFrame(data.data(), payloadDescriptorSize));
}
