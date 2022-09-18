#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "utils/SimpleJson.h"
#include <cstdint>
#include <gtest/gtest.h>

class BarbellMessagesTest : public ::testing::Test
{
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(BarbellMessagesTest, parseValidMinUplinkMessage)
{
    const auto message = R"({"type":"min-uplink-bitrate", "bitrateKbps":123456})";
    const auto json = utils::SimpleJson::create(message);

    ASSERT_TRUE(api::DataChannelMessageParser::isMinUplinkBitrate(json));
    ASSERT_EQ(api::DataChannelMessageParser::getMinUplinkBitrate(json), 123456);
}

TEST_F(BarbellMessagesTest, makeValidMinUplinkMessage)
{
    utils::StringBuilder<1024> message;
    api::DataChannelMessage::makeMinUplinkBitrate(message, 654321);
    const auto json = utils::SimpleJson::create(message.get());

    ASSERT_TRUE(api::DataChannelMessageParser::isMinUplinkBitrate(json));
    ASSERT_EQ(api::DataChannelMessageParser::getMinUplinkBitrate(json), 654321);
}
