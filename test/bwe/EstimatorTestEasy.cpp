#include "bwe/BandwidthEstimator.h"
#include "logger/Logger.h"
#include "math/Matrix.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "test/CsvWriter.h"
#include "test/bwe/FakeCall.h"
#include "test/bwe/FakeCrossTraffic.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/transport/NetworkLink.h"
#include "utils/Format.h"
#include <gtest/gtest.h>

using namespace math;

namespace rtp
{
uint32_t nsToSecondsFp6_18(uint64_t timestampNs);
}
TEST(BweTest, absTimestamp)
{
    uint64_t t1 = 0;
    auto ntp24 = rtp::nsToSecondsFp6_18(t1);
    EXPECT_EQ(ntp24, 0);
    uint64_t t2 = t1 + utils::Time::ms * 63999;
    ntp24 = rtp::nsToSecondsFp6_18(t2);
    EXPECT_EQ(ntp24, 16776953);
    uint64_t t3 = t1 + utils::Time::sec * 64;
    ntp24 = rtp::nsToSecondsFp6_18(t3);
    EXPECT_EQ(ntp24, 0);
    uint64_t t4 = t1 + utils::Time::ms * 65555;
    ntp24 = rtp::nsToSecondsFp6_18(t4);
    EXPECT_NE(ntp24, 0);
    EXPECT_EQ(ntp24, rtp::nsToSecondsFp6_18(utils::Time::ms * 1555));
}

TEST(BweTest, absTimestampExt)
{
    memory::Packet packet;
    auto header = rtp::RtpHeader::create(packet);
    rtp::RtpHeaderExtension ext;
    rtp::GeneralExtension1Byteheader timeExt(4, 3);
    auto cursor = ext.extensions().begin();
    ext.addExtension(cursor, timeExt);
    header->setExtensions(ext);
    packet.setLength(header->headerLength());

    uint64_t t1 = 0;
    rtp::setTransmissionTimestamp(packet, 4, t1);
    uint32_t sendTime = 0;
    rtp::getTransmissionTimestamp(packet, 4, sendTime);
    EXPECT_EQ(sendTime, 0);
    uint64_t t2 = t1 + utils::Time::ms * 63999;
    rtp::setTransmissionTimestamp(packet, 4, t2);
    rtp::getTransmissionTimestamp(packet, 4, sendTime);
    EXPECT_EQ(sendTime, 16776953);
    uint64_t t3 = t1 + utils::Time::sec * 64;
    rtp::setTransmissionTimestamp(packet, 4, t3);
    rtp::getTransmissionTimestamp(packet, 4, sendTime);
    EXPECT_EQ(sendTime, 0);
    uint64_t t4 = t1 + utils::Time::ms * 65555;
    rtp::setTransmissionTimestamp(packet, 4, t4);
    rtp::getTransmissionTimestamp(packet, 4, sendTime);
    EXPECT_EQ(sendTime, rtp::nsToSecondsFp6_18(utils::Time::ms * 1555));
}

TEST(BweTest, burstDelivery)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);
    fakenet::NetworkLink* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 200, 64 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    link->setBurstDeliveryInterval(45);

    fakenet::Call call(allocator, estimator, link, true, 60 * utils::Time::sec, "_ssdata/burstDelivery.csv");
    call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 50));
    while (call.run(utils::Time::sec))
    {
    }
}

TEST(BweTest, plainVideo)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);

    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 4800, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    // link->setBurstDeliveryInterval(45);

    auto* video = new fakenet::FakeVideoSource(allocator, 1220, 1);
    fakenet::Call call(allocator, estimator, link, true, 60 * utils::Time::sec, "_ssdata/plainVideo.csv");
    // call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 2750));
    call.addSource(video);
    while (call.run(utils::Time::sec))
    {
        logger::debug("link rate %fkbps", "", link->getBitRateKbps(call.getTime()));
        video->setBandwidth(std::min(1800.0, 0.8 * call.getEstimate()));
    }
}

TEST(BweTest, plainVideoLong)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);

    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 148000, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    // link->setBurstDeliveryInterval(45);

    auto* video = new fakenet::FakeVideoSource(allocator, 1220, 1);
    fakenet::Call call(allocator, estimator, link, true, 1200 * utils::Time::sec);
    // call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 2750));
    call.addSource(video);
    while (call.run(utils::Time::sec))
    {
        logger::debug("link rate %fkbps", "", link->getBitRateKbps(call.getTime()));
        video->setBandwidth(std::min(1800.0, 0.8 * call.getEstimate()));
    }
}

TEST(BweTest, plainVideoStartLow)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 3800, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    // link->setBurstDeliveryInterval(5);

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);

    fakenet::Call call(allocator, estimator, link, true, 60 * utils::Time::sec, "_ssdata/plainStartLow.csv");
    // call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 2750));
    call.addSource(video);
    while (call.run(utils::Time::sec))
    {
        video->setBandwidth(std::min(4000.0, 0.9 * call.getEstimate()));
    }
}

TEST(BweTest, plainVideoBwDrop)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 3800, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    link->setBurstDeliveryInterval(5);

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);

    fakenet::Call call(allocator, estimator, link, true, 300 * utils::Time::sec, "_ssdata/videoBwDrop.csv");
    // call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 2750));
    call.addSource(video);
    int count = 0;
    while (call.run(utils::Time::sec))
    {
        video->setBandwidth(std::min(4000.0, 0.9 * call.getEstimate()));
        if (count == 10)
        {
            link->setBandwidthKbps(1400);
        }

        ++count;
    }
}

TEST(BweTest, startCongested)
{
    bwe::Config config;
    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 800, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);
    video->setBandwidth(1200.0);

    fakenet::Call call(allocator, estimator, link, true, 500 * utils::Time::sec, "_ssdata/startCongested.csv");
    call.addSource(video);
    int count = 0;
    while (call.run(utils::Time::sec))
    {
        if (count == 2)
        {
            estimator.reset();
        }
        else if (count > 2)
        {
            video->setBandwidth(std::min(4000.0, 0.91 * call.getEstimate()));
        }

        if (count > 30)
        {
            // EXPECT_LT(estimator.getState()(2), 8.0);
        }
        count++;
    }
}

TEST(BweTest, networkPause)
{
    bwe::Config config;

    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 2000, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);
    video->setBandwidth(300);

    fakenet::Call call(allocator, estimator, link, true, 100 * utils::Time::sec, "_ssdata/networkPause.csv");
    call.addSource(video);
    int count = 0;
    while (call.run(utils::Time::sec))
    {
        video->setBandwidth(std::min(4000.0, 0.8 * call.getEstimate()));
        if (count == 15)
        {
            link->setBandwidthKbps(0);
        }
        if (count == 17)
        {
            EXPECT_EQ(call.getEstimate(), config.silence.maxBandwidthKbps);
            link->setBandwidthKbps(2000);
        }
        if (count > 25)
        {
            EXPECT_GE(call.getEstimate(), 1100);
        }

        count++;
    }
}

class BweEthernet : public testing::TestWithParam<uint32_t>
{
};

TEST_P(BweEthernet, lowBw)
{
    const double IPOH_MARGIN = 220.0 * 34 * 8 / 1000;
    bwe::Config config;
    config.measurementNoise *= 1.0;
    config.estimate.minReportedKbps = 200;
    config.estimate.minKbps = 150;

    const uint32_t linkBw = GetParam();

    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", linkBw, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    // auto* xtraffic = new fakenet::FakeCrossTraffic(allocator, 1150, linkBw * 0.07);

    auto video = new fakenet::FakeVideoSource(allocator, 150, 1);
    video->setBandwidth(200.0);

    // audio consuming 100kbps

    fakenet::Call call(allocator,
        estimator,
        link,
        true,
        500 * utils::Time::sec,
        utils::format("_ssdata/estLowBw%u.csv", linkBw).c_str());
    call.addSource(video);
    // call.addSource(xtraffic);
    int count = 0;

    while (call.run(utils::Time::sec / 8))
    {
        if ((count % 8) == 7)
        {
            auto ebw = estimator.getEstimate(call.getTime());
            // subtract bw for audio and xtraffic, IP overhead
            video->setBandwidth(std::max(0.0, 0.9 * std::min(ebw - IPOH_MARGIN - 125.0, 1160.0)));
            // EXPECT_LT(estimator.getState()(1), 1000.0);
        }
        count++;

        if (count > 6 * 8)
        {
            EXPECT_GT(estimator.getEstimate(call.getTime()), std::min(8500.0, linkBw * 0.8));
        }
    }
}

INSTANTIATE_TEST_SUITE_P(BweEthernetTest,
    BweEthernet,
    testing::Values(250, 300, 500, 800, 1200, 1500, 1800, 2100, 2700, 3500, 30000));