#include "IntegrationTest.h"
#include "SampleDataUtils.h"
#include "codec/Opus.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"

class StatsTest : public IntegrationTest
{
public:
    void SetUp() override
    {
        IntegrationTest::SetUp();
        startDefaultMixerManager();
    }

    template <class Predicate>
    bool waitForStats(Predicate pred)
    {
        for (int attmpt = 0; attmpt < 200; ++attmpt)
        {
            const auto value = mixerManager->getStats();
            if (pred(value))
            {
                return true;
            }
            usleep(10 * 1000);
        }
        return false;
    }
};

TEST_F(StatsTest, initialStatsTest)
{
    const auto value = mixerManager->getStats();
    usleep(1500000);
    EXPECT_GT(value.systemStats.processMemory, 0);
#ifndef __APPLE__
    EXPECT_GT(value.systemStats.totalNumberOfThreads, config->numWorkerTreads.get());
#endif
    EXPECT_EQ(value.numberOfMixers, 0);
    EXPECT_EQ(value.numberOfMixedLegs, 0);
    EXPECT_EQ(value.incomingBitRate, 0);
    EXPECT_EQ(value.incomingPacketsRate, 0);
    EXPECT_EQ(value.outgoingBitRate, 0);
    EXPECT_EQ(value.outgoingPacketsRate, 0);
    EXPECT_EQ(value.engine.timeSlipCount, 0);
}

TEST_F(StatsTest, statsWithRunningMixers)
{
    bridge::Mixer* mixer1 = mixerManager->create();
    mixer1->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) {
        return value.numberOfMixers == 1 && value.numberOfMixedLegs == 0;
    }));

    bridge::Mixer* mixer2 = mixerManager->create();
    mixer2->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) {
        return value.numberOfMixers == 2 && value.numberOfMixedLegs == 0;
    }));

    for (int i = 0; i < 20; ++i)
    {
        std::string mixedLegId;
        mixer1->addMixedLeg(mixedLegId, 0, true);
        mixer2->addMixedLeg(mixedLegId, 0, true);
    }

    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) {
        return value.systemStats.processCPU > 0 && value.engine.avgIdle < 100.0;
    }));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) { return value.numberOfMixedLegs == 40; }));

    mixerManager->remove(std::string(mixer1->getId()));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) { return value.numberOfMixers == 1; }));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) { return value.numberOfMixedLegs == 20; }));

    mixerManager->remove(std::string(mixer2->getId()));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) { return value.numberOfMixers == 0; }));
    EXPECT_TRUE(waitForStats([](const stats::MixerManagerStats& value) { return value.numberOfMixedLegs == 0; }));
}
