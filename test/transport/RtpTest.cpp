#include "config/Config.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/RtpSenderState.h"
#include "utils/Time.h"
#include <gtest/gtest.h>

struct RtcpTest : public ::testing::Test
{
    config::Config _config;
};

TEST_F(RtcpTest, SourceDescription)
{
    rtp::RtcpSourceDescription report;
    rtp::RtcpSourceDescription::Chunk _chunk0(105);

    rtp::SDESItem s(rtp::SDESItem::Type::CNAME, "harry.potter");
    rtp::SDESItem p(rtp::SDESItem::Type::PHONE, "+47123456");
    _chunk0.addItem(s);
    _chunk0.addItem(p);
    report.addChunk(_chunk0);
    EXPECT_TRUE(report.isValid());

    rtp::RtcpSourceDescription::Chunk _chunk1(106);
    _chunk1.addItem(p);
    rtp::SDESItem c(rtp::SDESItem::Type::CNAME);
    c.setValue("mumbo.jumbo");
    _chunk1.addItem(c);
    report.addChunk(_chunk1);

    EXPECT_TRUE(report.isValid());
    EXPECT_EQ(report.getChunkCount(), 2);
    auto chunkIt = report.cbegin();
    auto& chunk0 = *chunkIt;
    auto& chunk1 = *(++chunkIt);

    EXPECT_EQ(chunk1.ssrc, 106);
    auto itemIt = chunk1.cbegin();
    ++itemIt;
    EXPECT_EQ(itemIt->getValue(), "mumbo.jumbo");
    EXPECT_EQ(chunk1.size(), 32);

    EXPECT_EQ(chunk0.ssrc, 105);
    EXPECT_EQ(chunk0.size(), 32);
    EXPECT_EQ(chunk0.cbegin()->getValue(), "harry.potter");

    EXPECT_EQ(report.header.length, 16);
}

TEST_F(RtcpTest, SourceDescSymphony)
{
    rtp::RtcpSourceDescription report;
    rtp::RtcpSourceDescription::Chunk chunk(1001);
    rtp::SDESItem sdesItem(rtp::SDESItem::Type::CNAME);
    sdesItem.setValue("SymphonyMixer");

    chunk.addItem(sdesItem);
    report.addChunk(chunk);

    EXPECT_TRUE(report.isValid());
    EXPECT_EQ(report.header.length, 5);
}

TEST_F(RtcpTest, NTP)
{
    transport::RtpSenderState state(8000, _config);

    const auto start = utils::Time::getAbsoluteTime();
    memory::Packet p;
    auto* header = rtp::RtpHeader::create(p.get(), 4000);
    header->timestamp = 45000;
    header->sequenceNumber = 1001;
    header->ssrc = 110;
    p.setLength(320);
    state.onRtpSent(start, p);

    const auto wallClockNtp = utils::Time::toNtp(std::chrono::system_clock::now());
    rtp::RtcpSenderReport report;
    report.ssrc = 110;
    state.fillInReport(report, start + utils::Time::ms * 15, wallClockNtp);
    EXPECT_LE(report.rtpTimestamp, header->timestamp + 130);
    EXPECT_TRUE(report.isValid());
}

TEST_F(RtcpTest, SenderReport)
{
    transport::RtpSenderState state(8000, _config);
    const auto start = utils::Time::getAbsoluteTime();
    const auto timepoint = std::chrono::system_clock::now();
    memory::Packet p;
    auto* header = rtp::RtpHeader::create(p.get(), 4000);
    header->timestamp = 45000;
    header->sequenceNumber = 1001;
    header->ssrc = 110;
    p.setLength(320);

    state.onRtpSent(start, p);

    auto sendTimeSR = utils::Time::toNtp(timepoint + std::chrono::milliseconds(50));
    rtp::RtcpSenderReport report;
    state.fillInReport(report, start + utils::Time::ms * 50, sendTimeSR);

    auto receiveTime = sendTimeSR + (uint64_t(100) << 32) / 1000;

    rtp::RtcpReceiverReport receiverReport;
    receiverReport.ssrc = 111;
    auto& block1 = receiverReport.addReportBlock(report.ssrc);
    block1.setDelaySinceLastSR(20 * utils::Time::ms);
    block1.extendedSeqNoReceived = 100;
    block1.interarrivalJitter = 0;
    block1.lastSR = (report.getNtp() >> 16) & 0xFFFFFFFFu;
    block1.loss.setFractionLost(0.15);
    block1.loss.setCumulativeLoss(170000);
    block1.ssrc = report.ssrc;
    state.onReceiverBlockReceived(start + utils::Time::ms * 150, (receiveTime >> 16), receiverReport.reportBlocks[0]);

    auto reportSummary = state.getSummary();
    EXPECT_EQ(reportSummary.getRtt() / utils::Time::ms, 100 - 20);
    EXPECT_NEAR(reportSummary.lossFraction, 0.15, 0.01);
    EXPECT_EQ(reportSummary.lostPackets, 170000);
    EXPECT_TRUE(receiverReport.isValid());

    auto* rr = rtp::RtcpReceiverReport::fromPtr(&receiverReport, receiverReport.header.size());
    EXPECT_TRUE(rr != nullptr && rr->isValid());

    auto* sr = rtp::RtcpSenderReport::fromPtr(&report, report.size());
    EXPECT_TRUE(sr != nullptr && sr->isValid());
}

TEST_F(RtcpTest, padding)
{
    transport::RtpSenderState state(8000, _config);
    const auto timepoint = std::chrono::system_clock::now();
    const auto start = utils::Time::getAbsoluteTime();
    memory::Packet p;
    auto* header = rtp::RtpHeader::create(p.get(), 4000);
    header->timestamp = 45000;
    header->sequenceNumber = 1001;
    header->ssrc = 110;
    p.setLength(320);
    state.onRtpSent(start, p);

    uint8_t testArea[2048];
    auto& report = *rtp::RtcpSenderReport::create(testArea);
    report.ssrc = 110;
    state.fillInReport(report,
        start + utils::Time::ms * 15,
        utils::Time::toNtp(timepoint + std::chrono::milliseconds(15)));
    report.header.addPadding(5);
    EXPECT_LE(report.rtpTimestamp, header->timestamp + 130);
    EXPECT_TRUE(report.isValid());
}

TEST_F(RtcpTest, SourceDescSegF)
{
    using namespace rtp;
    RtcpSourceDescription report;
    RtcpSourceDescription::Chunk chunk(1001);

    chunk.addItem(SDESItem(SDESItem::Type::CNAME, "SymphonyMixer"));
    chunk.addItem(SDESItem(SDESItem::Type::CNAME, "SymphonyMoggler"));
    EXPECT_EQ(chunk.getCount(), 2);
    report.addChunk(chunk);
    auto it = report.begin();
    // make the last chunk pass outside header length
    it->addItem(SDESItem(SDESItem::Type::EMAIL, "blackhole.amazon@devastation.com"));
    EXPECT_EQ(it->getCount(), 3);

    EXPECT_FALSE(report.isValid());
    EXPECT_EQ(report.cbegin(), report.cend());
    EXPECT_EQ(report.header.length, 10);
}

TEST_F(RtcpTest, compound)
{
    using namespace rtp;
    RtcpSourceDescription report;
    RtcpSourceDescription::Chunk chunk(1001);

    rtp::RtcpReceiverReport receiverReport;
    receiverReport.ssrc = 111;
    auto& block1 = receiverReport.addReportBlock(112);
    block1.setDelaySinceLastSR(20 * 1000000ull);
    block1.extendedSeqNoReceived = 100;
    block1.interarrivalJitter = 0;
    block1.loss.setFractionLost(0.15);
    block1.loss.setCumulativeLoss(170000);

    EXPECT_EQ(block1.ssrc, 112);

    uint8_t memblock[512];
    std::memcpy(memblock, &receiverReport, receiverReport.header.size());
    std::memcpy(&memblock[receiverReport.header.size()], &report, report.header.size());

    CompoundRtcpPacket pkt(memblock, receiverReport.header.size() + report.header.size());
    auto it = pkt.begin();
    EXPECT_EQ(it->size(), receiverReport.header.size());
    EXPECT_EQ(it->packetType, RtcpPacketType::RECEIVER_REPORT);
    ++it;
    EXPECT_TRUE(it != pkt.end());
    EXPECT_EQ(it->packetType, RtcpPacketType::SOURCE_DESCRIPTION);
    ++it;
    EXPECT_TRUE(it == pkt.end());
}
