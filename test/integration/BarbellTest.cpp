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
#include "test/integration/emulator/Httpd.h"
#include "test/integration/emulator/SfuClient.h"
#include "transport/DataReceiver.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/Format.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"
#include <complex>
#include <memory>
#include <sstream>
#include <unordered_set>

BarbellTest::BarbellTest() {}

void BarbellTest::SetUp()
{
    _smbConfig1 = utils::format(R"({
        "ip":"127.0.0.1",
        "ice.publicIpv4":"%s",
        "rctl.enable": false,
        "bwe.enable":false
        })",
        _ipv4.smb.c_str());

    _smbConfig2 = utils::format(R"({
        "ip":"127.0.0.1",
        "ice.publicIpv4":"%s",
        "ice.singlePort":12000,
        "port":8090,
        "recording.singlePort":12500,
        "rctl.enable": false
        })",
        "35.240.203.95");

    _smb2interfaces.push_back(transport::SocketAddress::parse("35.240.203.95"));

    IntegrationTest::SetUp();
}
void BarbellTest::TearDown()
{
    IntegrationTest::TearDown();
}

using namespace emulator;

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
    runTestInThread(expectedTestThreadCount(2), [this]() {
        constexpr auto PACKET_LOSS_RATE = 0.03;

        _config.readFromString(_smbConfig1);

        initBridge(_config);

        config::Config config2;
        config2.readFromString(_smbConfig2);

        emulator::HttpdFactory httpd2;
        auto bridge2 = std::make_unique<bridge::Bridge>(config2);
        bridge2->initialize(_bridgeEndpointFactory, httpd2, _smb2interfaces);

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            // SFU's default downlinks is good (1 Gbps).
            linkInfo.second.ptrLink->setBandwidthKbps(1000000);
        }

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            0);
        group.add(_httpd);
        group.add(&httpd2);
        group.add(&httpd2);

        Conference conf(_httpd);
        group.startConference(conf, baseUrl);

        Conference conf2(&httpd2);
        group.startConference(conf2, baseUrl2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        Barbell bb1(_httpd);
        Barbell bb2(&httpd2);

        // The map _endpointNetworkLinkMap is populated automatically on endpoint creation.
        // Following it's evolution allows to find Endpoint/NetworkLink for particular component.
        _endpointNetworkLinkMap.clear();
        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto interBridgeEndpoints1 = _endpointNetworkLinkMap;

        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        for (const auto& linkInfo : interBridgeEndpoints1)
        {
            linkInfo.second.ptrLink->setLossRate(PACKET_LOSS_RATE);
            linkInfo.second.ptrLink->setBandwidthKbps(1000000);
        }

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto confRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(confRequest);

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

        for (auto id : {0, 1, 2})
        {
            std::string clientName = "Client-" + std::to_string(id + 1);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            const auto videoReceiveStats = group.clients[id]->_transport->getCumulativeVideoReceiveCounters();
            group.clients[id]->_transport->getReportSummary(transportSummary);

            logger::debug("%s received video pkts %" PRIu64, "bbTest", clientName.c_str(), videoReceiveStats.packets);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

            auto allStreamsVideoStats = group.clients[id]->getActiveVideoDecoderStats();
            EXPECT_EQ(allStreamsVideoStats.size(), 2);
            for (const auto& videoStats : allStreamsVideoStats)
            {
                const double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
                if (id == 0)
                {
                    EXPECT_NEAR(fps, 30.0, 5.0);
                    EXPECT_NEAR(videoStats.numDecodedFrames, 146, 11);
                }
                else
                {
                    EXPECT_NEAR(fps, 30.0, 1.0);
                    EXPECT_NEAR(videoStats.numDecodedFrames, 150, 11);
                }
            }
        }
    });
}

TEST_F(BarbellTest, simpleBarbell)
{
    runTestInThread(expectedTestThreadCount(2), [this]() {
        _config.readFromString(_smbConfig1);

        initBridge(_config);

        config::Config config2;
        config2.readFromString(_smbConfig2);

        emulator::HttpdFactory httpd2;
        auto bridge2 = std::make_unique<bridge::Bridge>(config2);
        bridge2->initialize(_bridgeEndpointFactory, httpd2, _smb2interfaces);

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            // SFU's default downlinks is good (1 Gbps).
            linkInfo.second.ptrLink->setBandwidthKbps(1000000);
        }

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            1);
        group.add(&httpd2);
        group.add(&httpd2);

        Conference conf(_httpd);
        Conference conf2(&httpd2);

        Barbell bb1(_httpd);
        Barbell bb2(&httpd2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto confRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(confRequest);

        bb1.remove(baseUrl);

        utils::Time::nanoSleep(utils::Time::ms * 1000); // let pending packets be sent and received)

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);
        logVideoSent("client3", *group.clients[2]);

        const double expectedFrequencies[2][2] = {{1300.0, 2100.0}, {600.0, 2100.0}};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
            EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
        }

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary1;
        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
        auto videoReceiveStats = group.clients[0]->_transport->getCumulativeVideoReceiveCounters();
        group.clients[1]->_transport->getReportSummary(transportSummary1);
        group.clients[2]->_transport->getReportSummary(transportSummary2);

        logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
        logVideoReceive("client1", *group.clients[0]);
        logTransportSummary("client2", group.clients[1]->_transport.get(), transportSummary1);
        logTransportSummary("client3", group.clients[2]->_transport.get(), transportSummary2);

        auto allStreamsVideoStats = group.clients[0]->getActiveVideoDecoderStats();
        EXPECT_EQ(allStreamsVideoStats.size(), 2);
        for (const auto& videoStats : allStreamsVideoStats)
        {
            const double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
            EXPECT_NEAR(fps, 30.0, 1.0);
            EXPECT_NEAR(videoStats.numDecodedFrames, 150, 5);
        }
    });
}

TEST_F(BarbellTest, barbellAfterClients)
{
    runTestInThread(expectedTestThreadCount(2), [this]() {
        _config.readFromString(_smbConfig1);

        initBridge(_config);

        config::Config config2;
        config2.readFromString(_smbConfig2);

        emulator::HttpdFactory httpd2;
        auto bridge2 = std::make_unique<bridge::Bridge>(config2);
        bridge2->initialize(_bridgeEndpointFactory, httpd2, _smb2interfaces);

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            // SFU's default downlinks is good (1 Gbps).
            linkInfo.second.ptrLink->setBandwidthKbps(1000000);
        }

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        utils::Time::nanoSleep(1 * utils::Time::sec);

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            1);
        group.add(&httpd2);

        Conference conf(_httpd);
        Conference conf2(&httpd2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true);

        if (!group.connectAll(utils::Time::sec * _clientsConnectionTimeout))
        {
            EXPECT_TRUE(false);
            return;
        }

        group.clients[0]->_audioSource->setFrequency(600);
        group.clients[1]->_audioSource->setFrequency(1300);

        group.clients[0]->_audioSource->setVolume(0.6);
        group.clients[1]->_audioSource->setVolume(0.6);

        utils::Time::nanoSleep(500 * utils::Time::ms);

        Barbell bb1(_httpd);
        Barbell bb2(&httpd2);

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.run(utils::Time::ms * 5000);

        group.clients[1]->stopRecording();
        group.clients[0]->stopRecording();

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto confRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(confRequest);

        utils::Time::nanoSleep(utils::Time::ms * 200); // let pending packets be sent and received
        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);

        const double expectedFrequencies[2] = {1300.0, 600.0};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_EQ(data.amplitudeProfile.size(), 2);
            if (data.amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(data.amplitudeProfile[1].second, 5725, 100);
            }
            ASSERT_GE(data.dominantFrequencies.size(), 1);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId++], 25.0);

            std::string clientName = "Client-" + std::to_string(id + 1);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            auto videoReceiveStats = group.clients[id]->_transport->getCumulativeVideoReceiveCounters();
            group.clients[id]->_transport->getReportSummary(transportSummary);

            logger::debug("%s received video pkts %" PRIu64, "bbTest", clientName.c_str(), videoReceiveStats.packets);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

            auto allStreamsVideoStats = group.clients[id]->getActiveVideoDecoderStats();
            EXPECT_EQ(allStreamsVideoStats.size(), 1);
            for (const auto& videoStats : allStreamsVideoStats)
            {
                double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
                EXPECT_NEAR(fps, 30.0, 1.0);
                EXPECT_NEAR(videoStats.numDecodedFrames, 150, 7);
            }
        }
    });
}

TEST_F(BarbellTest, barbellNeighbours)
{
    runTestInThread(expectedTestThreadCount(2), [this]() {
        _config.readFromString(_smbConfig1);

        initBridge(_config);

        config::Config smbConfig2;
        smbConfig2.readFromString(_smbConfig2);

        emulator::HttpdFactory httpd2;
        auto bridge2 = std::make_unique<bridge::Bridge>(smbConfig2);
        bridge2->initialize(_bridgeEndpointFactory, httpd2, _smb2interfaces);

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            // SFU's default downlinks is good (1 Gbps).
            linkInfo.second.ptrLink->setBandwidthKbps(1000000);
        }

        const auto baseUrl = "http://72.0.83.1:8080";
        const auto baseUrl2 = "http://72.0.83.2:8090";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            2);
        group.add(&httpd2);

        Conference conf(_httpd);
        Conference conf2(&httpd2);

        Barbell bb1(_httpd);
        Barbell bb2(&httpd2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);
        std::string neighbours[] = {"gid1"};
        utils::Span<std::string> span(neighbours);
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall2(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true, span);
        group.clients[2]->initiateCall2(baseUrl2, conf2.getId(), false, emulator::Audio::Opus, true, true, span);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto confRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(confRequest);

        bb1.remove(baseUrl);

        utils::Time::nanoSleep(utils::Time::ms * 1000); // let pending packets be sent and received)

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const size_t expectedFreqCount[] = {2, 1, 1};
        for (size_t id = 0; id < 3; ++id)
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), expectedFreqCount[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->_transport->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    });
}
