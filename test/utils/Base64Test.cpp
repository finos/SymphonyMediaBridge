#include "utils/Base64.h"
#include <gtest/gtest.h>

TEST(Base64, encode)
{
    std::string text = "Hello base64 world!";
    std::string strResult = utils::Base64::encode(reinterpret_cast<const uint8_t*>(text.c_str()), text.size());

    EXPECT_STREQ(strResult.c_str(), "SGVsbG8gYmFzZTY0IHdvcmxkIQ==");
}

TEST(Base64, decode)
{
    std::string encoded = "SGVsbG8gYmFzZTY0IHdvcmxkIQ==";
    uint8_t result[utils::Base64::decodeLength(encoded)];

    auto s = utils::Base64::decode(encoded, result, 19);
    EXPECT_EQ(s, 19);

    std::string strResult;
    for (const auto& value : result)
    {
        strResult += (char)value;
    }

    EXPECT_STREQ(strResult.c_str(), "Hello base64 world!");
}

TEST(Base64, decodeSaltExample)
{
    std::string encoded = "UXVpZCBwcm8gcXVv";

    uint8_t result[utils::Base64::decodeLength(encoded)];

    auto s = utils::Base64::decode(encoded, result, 12);
    EXPECT_EQ(s, 12);

    std::string strResult;
    for (const auto& value : result)
    {
        strResult += (char)value;
    }
    EXPECT_STREQ(strResult.c_str(), "Quid pro quo");
}
