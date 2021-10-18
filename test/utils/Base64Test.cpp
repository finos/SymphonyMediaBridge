#include "utils/Base64.h"
#include <gtest/gtest.h>

TEST(Base64, encode)
{
    std::string text = "Hello base64 world!";
    uint8_t result[utils::Base64::encodeLength(text)];

    utils::Base64::encode(text, result, 28);

    std::string strResult;
    for (const auto& value : result)
    {
        strResult += (char)value;
    }
    EXPECT_STREQ(strResult.c_str(), "SGVsbG8gYmFzZTY0IHdvcmxkIQ==");
}

TEST(Base64, decode)
{
    std::string encoded = "SGVsbG8gYmFzZTY0IHdvcmxkIQ==";
    uint8_t result[utils::Base64::decodeLength(encoded)];

    utils::Base64::decode(encoded, result, 19);

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

    utils::Base64::decode(encoded, result, 12);

    std::string strResult;
    for (const auto& value : result)
    {
        strResult += (char)value;
    }
    EXPECT_STREQ(strResult.c_str(), "Quid pro quo");
}
