#include "rtp/RtcpFeedback.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>

TEST(RtcpFeedbackTest, parsePacketNack)
{
    std::array<uint8_t, 32> nack;
    memset(nack.data(), 0, nack.size());

    auto rtcpFeedback = reinterpret_cast<rtp::RtcpFeedback*>(nack.data());
    rtcpFeedback->_header.packetType = rtp::RTPTRANSPORT_FB;
    rtcpFeedback->_header.fmtCount = rtp::TransportLayerFeedbackType::PacketNack;
    rtcpFeedback->_header.length = sizeof(rtp::RtcpFeedback) / 4 + 2 - 1;
    rtcpFeedback->_header.padding = 0;
    rtcpFeedback->_header.version = 2;
    rtcpFeedback->_mediaSsrc = 1;
    rtcpFeedback->_reporterSsrc = 2;

    auto feedbackControlInfo = nack.data() + sizeof(rtp::RtcpFeedback);
    feedbackControlInfo[0] = 0x12;
    feedbackControlInfo[1] = 0x67;
    feedbackControlInfo[2] = 0x1;
    feedbackControlInfo[3] = 0x3;

    feedbackControlInfo += 4;
    feedbackControlInfo[0] = 0x12;
    feedbackControlInfo[1] = 0x68;
    feedbackControlInfo[2] = 0x0;
    feedbackControlInfo[3] = 0x0;

    const auto numFeedbackControlInfos = rtp::getNumFeedbackControlInfos(rtcpFeedback);
    EXPECT_EQ(2, numFeedbackControlInfos);

    uint16_t pid = 0;
    uint16_t blp = 0;

    rtp::getFeedbackControlInfo(rtcpFeedback, 0, numFeedbackControlInfos, pid, blp);
    EXPECT_EQ(0x1267, pid);
    EXPECT_EQ(0x0103, blp);

    rtp::getFeedbackControlInfo(rtcpFeedback, 1, numFeedbackControlInfos, pid, blp);
    EXPECT_EQ(0x1268, pid);
    EXPECT_EQ(0x0000, blp);
}

TEST(RtcpFeedbackTest, Remb)
{
    uint8_t data[512];
    auto& remb = rtp::RtcpRembFeedback::create(data, 556677);
    EXPECT_TRUE(rtp::isRemb(&remb));
    EXPECT_EQ(remb.reporterSsrc, 556677);
    remb.addSsrc(45);
    EXPECT_EQ(remb.ssrcFeedback[0], 45);
    EXPECT_EQ(remb.ssrcCount, 1);
    const uint64_t bps = uint64_t(955) * 8 * 1000000;
    EXPECT_EQ(remb.getBitRate(), 0);
    remb.setBitRate(bps);
    EXPECT_NEAR(remb.getBitRate(), bps, 10000);
    EXPECT_EQ(remb.header.length.get(), 4 + 1); // only one ssrc added
    EXPECT_EQ(remb.header.size(), sizeof(remb) + 4);
}