#include "bwe/RateController.h"
#include "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/bwe/RcCall.h"
#include "test/transport/FakeNetwork.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "transport/ice/IceSession.h"
#include "transport/sctp/SctpConfig.h"
#include <gtest/gtest.h>
#include <vector>

class RateControllerTestBase : public ::testing::TestWithParam<uint32_t>
{
public:
    RateControllerTestBase() : _allocator(4096 * 32, "ratemain") {}

    memory::PacketPoolAllocator _allocator;
    config::Config _config;
};

class RateControllerTestLongRtt : public RateControllerTestBase
{
};

TEST_P(RateControllerTestLongRtt, longRtt)
{
    uint32_t capacityKbps = GetParam();
    bwe::RateControllerConfig rcConfig;
    rcConfig.initialEstimateKbps = 300;
    rcConfig.debugLog = true;
    bwe::RateController rateControl(1, rcConfig);
    auto* uplink = new fakenet::NetworkLink(capacityKbps, 75000, 1480);
    uplink->setStaticDelay(90);
    auto* downLink = new fakenet::NetworkLink(6500, 75000, 1480);
    downLink->setStaticDelay(70);

    fakenet::RcCall call(_allocator, _config, rateControl, uplink, downLink, true, 24 * utils::Time::sec);

    fakenet::FakeVideoSource* video = nullptr;
    if (capacityKbps > 300)
    {
        auto video = new fakenet::FakeVideoSource(_allocator, 0, 10);
        call.addChannel(video, 90000);
        video->setBandwidth(100);
    }

    call.run(utils::Time::sec * 16, capacityKbps * 1.05);

    EXPECT_GE(rateControl.getTargetRate(), std::min(1000.0, capacityKbps * 0.60));
    if (video)
    {
        video->setBandwidth((rateControl.getTargetRate() - 100) * 0.8);
    }
    double timeLine = 0;
    while (call.run(utils::Time::sec * 2, capacityKbps * 1.05))
    {
        timeLine += 2;
        if (video)
        {
            video->setBandwidth(std::min(1800.0, (rateControl.getTargetRate() - 100) * 0.8));
        }
        EXPECT_GE(rateControl.getTargetRate(), std::min(1000 + (timeLine * 200), capacityKbps * 0.90));
        EXPECT_LT(rateControl.getTargetRate(), capacityKbps * 1.05);
    }
}

INSTANTIATE_TEST_SUITE_P(RateControllerLongRtt,
    RateControllerTestLongRtt,
    testing::Values(300, 500, 700, 1000, 1200, 3000, 4000, 5000));

class RateControllerTestShortRtt : public RateControllerTestBase
{
};

TEST_P(RateControllerTestShortRtt, shortRtt)
{
    uint32_t capacityKbps = GetParam();
    bwe::RateControllerConfig rcConfig;
    rcConfig.initialEstimateKbps = 300;
    rcConfig.debugLog = true;
    bwe::RateController rateControl(1, rcConfig);
    auto* uplink = new fakenet::NetworkLink(capacityKbps, 75000, 1480);
    uplink->setStaticDelay(0);
    auto* downLink = new fakenet::NetworkLink(6500, 75000, 1480);
    downLink->setStaticDelay(0);

    fakenet::RcCall call(_allocator, _config, rateControl, uplink, downLink, true, 24 * utils::Time::sec);

    fakenet::FakeVideoSource* video = nullptr;
    if (capacityKbps > 300)
    {
        auto video = new fakenet::FakeVideoSource(_allocator, 0, 10);
        call.addChannel(video, 90000);
        video->setBandwidth(100);
    }

    call.run(utils::Time::sec * 10, capacityKbps * 1.05);
    double timeLine = 0;
    EXPECT_GE(rateControl.getTargetRate(), std::min(1000.0, capacityKbps * 0.60));
    if (video)
    {
        video->setBandwidth((rateControl.getTargetRate() - 100) * 0.8);
    }
    while (call.run(utils::Time::sec * 2, capacityKbps * 1.05))
    {
        timeLine += 2;
        if (video)
        {
            video->setBandwidth(std::min(1800.0, (rateControl.getTargetRate() - 100) * 0.8));
        }
        EXPECT_GE(rateControl.getTargetRate(), std::min(1000 + (timeLine * 150), capacityKbps * 0.90));
        EXPECT_LT(rateControl.getTargetRate(), capacityKbps * 1.05);
    }
}

INSTANTIATE_TEST_SUITE_P(RateControllerShortRtt,
    RateControllerTestShortRtt,
    testing::Values(300, 500, 700, 1000, 1200, 3000, 4000, 5000));
