#include "IntegrationTest.h"
#include "SampleDataUtils.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"

struct MixerTimeoutTest : public IntegrationTest
{
    const int testInactivityTimeoutMs = 1000;

    void SetUp() override
    {
        IntegrationTest::SetUp();
        startDefaultMixerManager();
        config->readFromString("{ \"mixerInactivityTimeoutMs\": " + std::to_string(testInactivityTimeoutMs) + " }");
    }
};

TEST_F(MixerTimeoutTest, mixerMustBeKeptAliveByMultilegIncomingPackets)
{
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    const std::string mixerId = mixer->getId();
    mixer->connectMultiLeg();
    TransportStub* multilegTransport = transportFactory->deqeue();
    multilegTransport->waitReady();

    const auto rtpStream = SampleDataUtils::generateOpusRtpStream((testInactivityTimeoutMs + 500) /
                                                                  bridge::EngineMixer::iterationDurationMs);
    for (size_t i = 0; i < rtpStream.size(); ++i)
    {
        multilegTransport->emulateIncomingPacket(rtpStream[i], 123);
        usleep(1000 * bridge::EngineMixer::iterationDurationMs);
        EXPECT_TRUE(!!mixerManager->getMixer(mixerId));
    }
}

TEST_F(MixerTimeoutTest, mixerMustBeKeptAliveByMixedLegIncomingPackets)
{
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    const std::string mixerId = mixer->getId();
    mixer->connectMultiLeg();
    transportFactory->deqeue();

    std::string mixedLegId;
    EXPECT_TRUE(mixer->addMixedLeg(mixedLegId, 0, true));
    TransportStub* mixedLegClientTransport = transportFactory->deqeue();
    const auto mixedLegIdSsrc = 123;
    mixer->setMixedLegClientAudio(
        mixedLegId, bridge::RtpMap::opus(), mixedLegIdSsrc, transport::MediaDirection::sendRecv, true);
    mixer->setMixedLegOutboundAudio(mixedLegId, bridge::RtpMap::opus(), 1);
    mixer->addMixedLegToEngine(mixedLegId);

    mixedLegClientTransport->waitReady();

    const auto rtpStream = SampleDataUtils::generateOpusRtpStream((testInactivityTimeoutMs + 500) /
                                                                  bridge::EngineMixer::iterationDurationMs);
    for (size_t i = 0; i < rtpStream.size(); ++i)
    {
        mixedLegClientTransport->emulateIncomingPacket(rtpStream[i], mixedLegIdSsrc);
        usleep(1000 * bridge::EngineMixer::iterationDurationMs);
        EXPECT_TRUE(!!mixerManager->getMixer(mixerId));
    }
}

TEST_F(MixerTimeoutTest, mixerMustBeGarbageCollectedIfNoIncomingPacketsCameAfterCreation)
{
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    const std::string mixerId = mixer->getId();
    mixer->connectMultiLeg();

    std::string mixedLegId;
    EXPECT_TRUE(mixer->addMixedLeg(mixedLegId, 0, true));

    mixer->setMixedLegClientAudio(mixedLegId, bridge::RtpMap::opus(), 123, transport::MediaDirection::sendRecv, true);
    mixer->setMixedLegOutboundAudio(mixedLegId, bridge::RtpMap::opus(), -1);
    mixer->addMixedLegToEngine(mixedLegId);
    EXPECT_TRUE(!!mixerManager->getMixer(mixerId));

    // If engine pacer slips, it will reset and not call enginemixer process inbound as often as intended.
    // This will cause longer period to detect no incoming rtp
    usleep(1000 * (testInactivityTimeoutMs + 2000));
    EXPECT_TRUE(!mixerManager->getMixer(mixerId));
}

TEST_F(MixerTimeoutTest, mixerMustBeGarbageCollectedWhenIncomingPacketsFlowStops)
{
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    const std::string mixerId = mixer->getId();
    mixer->connectMultiLeg();
    TransportStub* multilegTransport = transportFactory->deqeue();
    multilegTransport->waitReady();

    multilegTransport->emulateIncomingPacket(SampleDataUtils::getOpusRtpSamplePackets()[0], 123);
    EXPECT_TRUE(!!mixerManager->getMixer(mixerId));

    usleep(1000 * (testInactivityTimeoutMs + 500));
    EXPECT_TRUE(!mixerManager->getMixer(mixerId));
}
