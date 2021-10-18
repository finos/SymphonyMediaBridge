#include "crypto/SslHelper.h"
#include <gtest/gtest.h>

TEST(Crc32, basic)
{
    crypto::Crc32Polynomial polynomial(0x04C11DB7);
    crypto::Crc32 crc(polynomial);
    auto data = (const unsigned char*)"ABC";
    crc.add(data, 3);
    EXPECT_EQ(crc.compute(), 0xa3830348u);
}

TEST(MD5, msgintegrity)
{
    const char* userpwd = "user:realm:pass";
    auto up = reinterpret_cast<const uint8_t*>(userpwd);

    crypto::MD5 md5;
    md5.add(up, strlen(userpwd));
    uint8_t md5hash[16];
    md5.compute(md5hash);
    auto s = crypto::toHexString(md5hash, 16);
    EXPECT_EQ(s, "8493fbc53ba582fb4c044c456bdc40eb");
}
