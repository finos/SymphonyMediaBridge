#include "IntegrationTest.h"
#include "SampleDataUtils.h"
#include "codec/Opus.h"
#include "concurrency/EventSemaphore.h"
#include "rtp/RtpHeader.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"

struct MixedLegManagementTest : public IntegrationTest
{
};

TEST_F(MixedLegManagementTest, canDeleteMixedLegDuringPacketsProcessing)
{
    startDefaultMixerManager();
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    mixer->connectMultiLeg();
    transportFactory->deqeue();

    std::string mixedLegId;
    EXPECT_TRUE(mixer->addMixedLeg(mixedLegId, 0, true, ice::IceRole::CONTROLLING));

    TransportStub* mixedLegClientTransport = transportFactory->deqeue();
    transportFactory->deqeue();
    std::mutex mutex;
    concurrency::EventSemaphore mixedLegWasStopped;
    mixedLegClientTransport->setStopHandler([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        mixedLegWasStopped.post();
    });
    const uint32_t mixedLegIdSsrc = 123123;
    mixer->setMixedLegClientAudio(
        mixedLegId, bridge::RtpMap::opus(), mixedLegIdSsrc, transport::MediaDirection::sendRecv, true);
    mixer->setMixedLegOutboundAudio(mixedLegId, bridge::RtpMap::opus(), 1);
    mixer->addMixedLegToEngine(mixedLegId);
    mixedLegClientTransport->waitReady();

    const auto rtpStream = SampleDataUtils::generateOpusRtpStream(10000);
    for (size_t i = 0; i < rtpStream.size(); ++i)
    {
        usleep(10);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (mixedLegWasStopped.await(0))
            {
                break;
            }
            mixedLegClientTransport->emulateIncomingPacket(rtpStream[i], mixedLegIdSsrc);
        }
        if (i == 100)
        {
            mixer->removeMixedLeg(mixedLegId);
        }
    }
    mixedLegWasStopped.await();
}

/*
    When the engine mixer has produced mixed leg data, those are posted on the
    serial job manager for encoding in encode jobs.
    When the encode job is complete, it will post another job on the serial manager to
    encrypt and send it. While encoding, the Mixer may be stopped and this will cause a goodbye message
    to be inserted in the serial job manager queue before the protectJob is created and posted.
*/
TEST_F(MixedLegManagementTest, rtcpGoodbyeMustBeSentWhenMixedLegIsDeleted)
{
    startDefaultMixerManager();
    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    mixer->connectMultiLeg();
    transportFactory->deqeue();

    std::string mixedLegId;
    EXPECT_TRUE(mixer->addMixedLeg(mixedLegId, 0, true, ice::IceRole::CONTROLLING));

    TransportStub* mixedLegClientTransport = transportFactory->deqeue();
    transportFactory->deqeue();

    mixer->setMixedLegClientAudio(
        mixedLegId, bridge::RtpMap::opus(), 123123, transport::MediaDirection::sendRecv, true);
    mixer->addMixedLegToEngine(mixedLegId);
    mixedLegClientTransport->waitReady();

    concurrency::EventSemaphore mixedLegWasStopped;
    mixedLegClientTransport->setStopHandler([&]() {
        auto packets = mixedLegClientTransport->getSentPacketsAndReset();
        EXPECT_GE(packets.size(), 1);
        bool byeFound = false;
        for (auto packet : packets)
        {
            rtp::RtcpHeader* packetHeader = rtp::RtcpHeader::fromPacket(packet);
            if (rtp::RtcpPacketType::GOODBYE == static_cast<rtp::RtcpPacketType>(packetHeader->packetType))
            {
                byeFound = true;
                break;
            }
        }
        EXPECT_TRUE(byeFound);
        mixedLegWasStopped.post();
    });

    mixer->removeMixedLeg(mixedLegId);

    mixedLegWasStopped.await();
}

TEST_F(MixedLegManagementTest, canAbortMixedLegSetup)
{
    ice::IceConfig iceConfig;
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse("127.0.0.1"));

    const auto srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*sslDtls);
    const auto network = std::unique_ptr<transport::RtcePoll>(transport::createRtcePoll());
    const auto realTransportFactory =
        transport::createTransportFactory(*jobManager, *srtpClientFactory, *config, iceConfig, interfaces, *network);

    mixerManager = std::make_unique<bridge::MixerManager>(
        *sslDtls, *idGenerator, *ssrcGenerator, *jobManager, *realTransportFactory, *engine, *config);

    bridge::Mixer* mixer = mixerManager->create();
    mixer->setMultiLegAudioRtpMap(bridge::RtpMaps(bridge::RtpMap::opus()));
    mixer->connectMultiLeg();

    std::string mixedLegId;
    EXPECT_TRUE(mixer->addMixedLeg(mixedLegId, 0, true, ice::IceRole::CONTROLLING));

    // we abort mixed leg setup
    mixer->removeMixedLeg(mixedLegId);
    mixerManager->remove(mixer->getId());
    utils::Time::nanoSleep(1500 * utils::Time::ms); // allow rtcepoll to sense transport stop
}