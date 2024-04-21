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

TEST(BweTest, basic)
{
    bwe::Config config;

    fakenet::NetworkLink* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 5000, 64 * 1024, 1500);
    memory::PacketPoolAllocator allocator(512, "test");

    bwe::BandwidthEstimator estimator(config);
    fakenet::Call call(allocator, estimator, link, true, 60 * utils::Time::sec, "_ssdata/basic.csv");
    call.addSource(new fakenet::FakeCrossTraffic(allocator, 1400, 150));
    while (call.run(utils::Time::sec))
    {
    }
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
        const auto state = estimator.getState();
        logger::debug("%ds: ukf estimate %.0f, %.0f, %.3f, rx %.0f",
            "",
            count + 1,
            state(1),
            state(0) / 8,
            state(2),
            estimator.getReceiveRate(call.getTime()));
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
            EXPECT_LT(estimator.getState()(2), 8.0);
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
        const auto state = estimator.getState();
        logger::debug("%ds: ukf estimate %.0f, %.0f, %.3f, rx %.0f",
            "",
            count + 1,
            state(1),
            state(0) / 8,
            state(2),
            estimator.getReceiveRate(call.getTime()));
        video->setBandwidth(std::min(4000.0, 0.9 * call.getEstimate()));
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

TEST(BweTest, lowBandwidth)
{
    const double IPOH_MARGIN = 200.0 * 34 * 8 / 1000;
    bwe::Config config;
    config.measurementNoise *= 1.0;
    config.estimate.minReportedKbps = 200;

    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 800, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);
    video->setBandwidth(200.0);

    // audio consuming 100kbps
    fakenet::Call call(allocator, estimator, link, true, 500 * utils::Time::sec, "_ssdata/estLowBw.csv");
    call.addSource(video);
    int count = 0;

    while (call.run(utils::Time::sec / 8))
    {
        const auto state = estimator.getState();
        logger::debug("%ds: ukf estimate %.0f, %.0f, %.6f, rx %.0f",
            "",
            count + 1,
            state(1),
            state(0) / 8,
            state(2),
            estimator.getReceiveRate(call.getTime()));

        if ((count % 8) == 7)
        {
            auto ebw = estimator.getEstimate(call.getTime());
            video->setBandwidth(std::min(ebw - IPOH_MARGIN - 100.0, 560.0));
            // EXPECT_LT(estimator.getState()(1), 1000.0);
        }
        count++;
    }
}

TEST(BweTest, lowBw300)
{
    const double IPOH_MARGIN = 200.0 * 34 * 8 / 1000;
    bwe::Config config;
    config.measurementNoise *= 1.0;
    config.estimate.minReportedKbps = 200;

    bwe::BandwidthEstimator estimator(config);
    auto* link = new fakenet::NetworkLink("EstimatorTestEasyLink", 350, 256 * 1024, 1500);
    memory::PacketPoolAllocator allocator(1024, "test");

    auto* video = new fakenet::FakeVideoSource(allocator, 150, 1);
    video->setBandwidth(200.0);

    // audio consuming 100kbps
    fakenet::Call call(allocator, estimator, link, true, 500 * utils::Time::sec, "_ssdata/estLowBw300.csv");
    call.addSource(video);
    int count = 0;

    while (call.run(utils::Time::sec / 8))
    {
        const auto state = estimator.getState();
        logger::debug("%ds: ukf estimate %.0f, %.0f, %.6f, rx %.0f",
            "",
            count + 1,
            state(1),
            state(0) / 8,
            state(2),
            estimator.getReceiveRate(call.getTime()));

        if ((count % 8) == 7)
        {
            auto ebw = estimator.getEstimate(call.getTime());
            video->setBandwidth(std::min(ebw - IPOH_MARGIN - 100.0, 560.0));
            // EXPECT_LT(estimator.getState()(1), 1000.0);
        }
        count++;
    }
}