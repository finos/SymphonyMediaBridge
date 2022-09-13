#include "BarbellTest.h"

#include "api/ConferenceEndpoint.h"
#include "api/Parser.h"
#include "api/utils.h"
#include "bridge/Mixer.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcHashmap.h"
#include "emulator/FakeEndpointFactory.h"
#include "external/http.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/bwe/FakeVideoSource.h"
#include "test/integration/IntegrationTest.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/HttpRequests.h"
#include "test/integration/emulator/SfuClient.h"
#include "transport/DataReceiver.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"
#include <complex>
#include <memory>
#include <sstream>
#include <unordered_set>

BarbellTest::BarbellTest() {}

void BarbellTest::SetUp()
{
    IntegrationTest::SetUp();
}
void BarbellTest::TearDown()
{
    IntegrationTest::TearDown();
}

namespace
{
template <typename T>
void logVideoSent(const char* clientName, T& client)
{
    for (auto& itPair : client._videoSources)
    {
        auto& videoSource = itPair.second;
        logger::info("%s video source %u, sent %u packets",
            "bbTest",
            clientName,
            videoSource->getSsrc(),
            videoSource->getPacketsSent());
    }
}

template <typename T>
void logTransportSummary(const char* clientName, transport::RtcTransport* transport, T& summary)
{
    for (auto& report : summary)
    {
        logger::debug("%s %s ssrc %u sent video pkts %u",
            "bbTest",
            clientName,
            transport->getLoggableId().c_str(),
            report.first,
            report.second.packetsSent);
    }
}
} // namespace

using namespace emulator;

template <typename TChannel>
void make5secCallWithDefaultAudioProfile(GroupCall<SfuClient<TChannel>>& groupCall)
{
    static const double frequencies[] = {600, 1300, 2100, 3200, 4100, 4800, 5200};
    for (size_t i = 0; i < groupCall.clients.size(); ++i)
    {
        groupCall.clients[i]->_audioSource->setFrequency(frequencies[i]);
    }

    for (auto& client : groupCall.clients)
    {
        client->_audioSource->setVolume(0.6);
    }

    groupCall.run(utils::Time::sec * 5);
    utils::Time::nanoSleep(utils::Time::sec * 1);

    for (auto& client : groupCall.clients)
    {
        client->stopRecording();
    }
}

/*
Test setup:
1. Topology:
                                                          /<----> Client-2
            Client-1 <----> Barbell-1 <------> Barbell-2 <
                                                          \<----> Client-3
2. Control:
   Barbell-1 inbound networklink is set to lose packets (e.g. 1%) that comes from Barbell-2.
3. Expectations:
   - Client-1 have audio packet loss, because they are NOT retransmitted.
   - Client-1 have NO video packet loss, because they ARE retransmitted.
*/
TEST_F(BarbellTest, packetLossViaBarbell)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        constexpr auto PACKET_LOSS_RATE = 0.01;

        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "rctl.enable": false,
        "bwe.enable":false
        })");

        initBridge(_config);

        config::Config config1;
        config1.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "rctl.enable": false
        })");

        config::Config config2;
        config2.readFromString(
            R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "ice.singlePort":12000,
        "port":8090,
        "recording.singlePort":12500,
        "rctl.enable": false
        })");

        auto bridge2 = std::make_unique<bridge::Bridge>(config2);
        bridge2->initialize(_bridgeEndpointFactory);

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;
        group.startConference(conf, baseUrl);

        Conference conf2;
        group.startConference(conf2, baseUrl2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        Barbell bb1;
        Barbell bb2;

        // This map: _endpointNetworkLinkMap is populated automatically on endpoint creation.
        // Following it's evolution allows to find Endpoint/NetworkLink for paritcular component.
        _endpointNetworkLinkMap.clear();
        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto interBridgeEndpoints1 = _endpointNetworkLinkMap;

        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        for (const auto& linkInfo : interBridgeEndpoints1)
        {
            linkInfo.second.ptrLink->setLossRate(PACKET_LOSS_RATE);
        }

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl2, conf2.getId(), false, true, true, true);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * 7));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        bb1.remove(baseUrl);

        utils::Time::nanoSleep(utils::Time::ms * 1000); // let pending packets be sent and received)

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.disconnectClients();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);
        logVideoSent("client3", *group.clients[2]);

        // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
        // video streams only. So let's check SfuClient NACK-related stats instead:

        const auto stats = group.clients[0]->getCumulativeRtxStats();
        auto videoCounters = group.clients[0]->_transport->getCumulativeVideoReceiveCounters();

        // Could happen that a key frame was sent after the packet that would be lost, in this case NACK would
        // have been ignored. So we might expect small number of videoCounters.lostPackets.
        if (videoCounters.lostPackets != 0)
        {
            ASSERT_TRUE(stats.receiver.packetsMissing >= stats.receiver.packetsRecovered);
            // Expect number of non-recovered packet to be smaller than half the loss rate.
            //                ASSERT_TRUE(stats.receiver.packetsMissing - stats.receiver.packetsRecovered <
            //                  stats.sender.packetsSent * PACKET_LOSS_RATE / 2);
        }

        // Assure that losses indeed happened.
        EXPECT_NE(stats.receiver.packetsMissing, 0);
        EXPECT_NE(stats.receiver.packetsRecovered, 0);

        /*        const auto& rData1 = group.clients[0]->getAudioReceiveStats();
                std::vector<double> allFreq;
                EXPECT_EQ(rData1.size(), 2);

                for (const auto& item : rData1)
                {
                    if (group.clients[0]->isRemoteVideoSsrc(item.first))
                    {
                        continue;
                    }

                    std::vector<double> freqVector;
                    std::vector<std::pair<uint64_t, double>> amplitudeProfile;
                    auto rec = item.second->getRecording();
                    analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());

                    allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

                    EXPECT_EQ(amplitudeProfile.size(), 2);
                    if (amplitudeProfile.size() > 1)
                    {
                        EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
                    }

                    // item.second->dumpPcmData();
                }

                std::sort(allFreq.begin(), allFreq.end());
                ASSERT_GE(allFreq.size(), 2);
                EXPECT_NEAR(allFreq[0], 1300.0, 25.0);
                EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
        */
        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary3;
        auto videoReceiveStats = group.clients[0]->_transport->getCumulativeVideoReceiveCounters();
        group.clients[1]->_transport->getReportSummary(transportSummary2);
        group.clients[2]->_transport->getReportSummary(transportSummary3);

        logger::debug("client1 received video pkts %" PRIu64 " lost %" PRIu64,
            "bbTest",
            videoReceiveStats.packets,
            videoReceiveStats.lostPackets);
        logTransportSummary("client2", group.clients[1]->_transport.get(), transportSummary2);
        logTransportSummary("client3", group.clients[2]->_transport.get(), transportSummary3);
        EXPECT_GE(group.clients[0]->getVideoPacketsReceived(),
            transportSummary2.begin()->second.packetsSent + transportSummary3.begin()->second.packetsSent - 100);
        EXPECT_NEAR(group.clients[0]->getVideoPacketsReceived(),
            transportSummary2.begin()->second.packetsSent + transportSummary3.begin()->second.packetsSent,
            200);
    });
}
