#include "utils/SocketAddress.h"
#include <gtest/gtest.h>
#include <string>

using namespace transport;

namespace
{
struct LogLevelSetAndRestore
{
    LogLevelSetAndRestore(logger::Level level) : _levelToRestore(logger::_logLevel) { logger::_logLevel = level; }
    ~LogLevelSetAndRestore() { logger::_logLevel = _levelToRestore; }

private:
    logger::Level _levelToRestore;
};

} // namespace

TEST(SocketAddressTest, toFixedStringIpv4)
{
    const auto socketAddress = SocketAddress::parse("127.0.0.1", 1096);
    auto fixedStringSocket = socketAddress.toFixedString();

    ASSERT_STREQ("127.0.0.1:1096", fixedStringSocket.c_str());
    ASSERT_EQ(std::strlen("127.0.0.1:1096"), fixedStringSocket.size());
}

TEST(SocketAddressTest, toFixedStringIpv6)
{
    const auto socketAddress0 = SocketAddress::parse("2001:0000:130F:0000:0000:09C0:876A:130B", 65093);
    auto fixedStringSocket0 = socketAddress0.toFixedString();
    ASSERT_STREQ("2001:0:130f::9c0:876a:130b:65093", fixedStringSocket0.c_str());
    ASSERT_EQ(std::strlen("2001:0:130f::9c0:876a:130b:65093"), fixedStringSocket0.size());

    const auto socketAddress1 = SocketAddress::parse("0:0:0:0:0:0:0:1", 9);
    auto fixedStringSocket1 = socketAddress1.toFixedString();
    ASSERT_STREQ("::1:9", fixedStringSocket1.c_str());
    ASSERT_EQ(std::strlen("::1:9"), fixedStringSocket1.size());
}

TEST(SocketAddressTest, maybeMaskShouldNotMaskWhenLogLevelIsDebug)
{
    LogLevelSetAndRestore l(logger::Level::DBG);

    const auto ipv6Address = SocketAddress::parse("2001:0:130f::9c0:876a:130b", 1000);
    const auto ipv4Address = SocketAddress::parse("64.63.123.98", 2665);

    auto maybeMaskedIpv6 = maybeMasked(ipv6Address);
    auto maybeMaskedIpv4 = maybeMasked(ipv4Address);

    ASSERT_STREQ("2001:0:130f::9c0:876a:130b:1000", maybeMaskedIpv6.c_str());
    ASSERT_EQ(std::strlen("2001:0:130f::9c0:876a:130b:1000"), maybeMaskedIpv6.size());

    ASSERT_STREQ("64.63.123.98:2665", maybeMaskedIpv4.c_str());
    ASSERT_EQ(std::strlen("64.63.123.98:2665"), maybeMaskedIpv4.size());
}

TEST(SocketAddressTest, maybeMaskShouldMaskWhenLogLevelIsNotDebug)
{
    std::array<logger::Level, 3> notDebugLevels = {
        logger::Level::ERROR,
        logger::Level::INFO,
        logger::Level::WARN,
    };

    const auto ipv6Address = SocketAddress::parse("2001:0:130f::9c0:876a:130b", 1000);
    const auto ipv4Address = SocketAddress::parse("64.63.123.98", 2665);

    for (const auto level : notDebugLevels)
    {
        LogLevelSetAndRestore l(level);

        auto maybeMaskedIpv6 = maybeMasked(ipv6Address);
        auto maybeMaskedIpv4 = maybeMasked(ipv4Address);

        const char* a = maybeMaskedIpv6.c_str();
        (void)a;

        ASSERT_STREQ("ipv6-6134717025100730244:1000", maybeMaskedIpv6.c_str());
        ASSERT_EQ(std::strlen("ipv6-6134717025100730244:1000"), maybeMaskedIpv6.size());

        ASSERT_STREQ("ipv4-14244227746646367573:2665", maybeMaskedIpv4.c_str());
        ASSERT_EQ(std::strlen("ipv4-14244227746646367573:2665"), maybeMaskedIpv4.size());
    }
}
