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
#include "math.h"
#include "memory/PacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/bwe/FakeVideoSource.h"
#include "test/integration/IntegrationTest.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/Barbell.h"
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
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
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
            *_publicTransportFactory,
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
        CallConfigBuilder cfg1(conf.getId());
        cfg1.url(baseUrl).av();

        CallConfigBuilder cfg2(cfg1);
        cfg2.room(conf2.getId()).url(baseUrl2);

        group.clients[0]->initiateCall(cfg1.build());
        group.clients[1]->joinCall(cfg2.build());
        group.clients[2]->joinCall(cfg2.build());

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

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.disconnectClients();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);
        logVideoSent("client3", *group.clients[2]);

        // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
        // video streams only. So let's check SfuClient NACK-related stats instead:

        const auto stats = group.clients[0]->getCumulativeRtxStats();
        auto videoCounters = group.clients[0]->getCumulativeVideoReceiveCounters();

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
            const auto videoReceiveStats = group.clients[id]->getCumulativeVideoReceiveCounters();
            group.clients[id]->getReportSummary(transportSummary);

            logger::debug("%s received video pkts %" PRIu64, "bbTest", clientName.c_str(), videoReceiveStats.packets);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
            logTransportSummary(clientName.c_str(), transportSummary);

            auto allStreamsVideoStats = group.clients[id]->getActiveVideoDecoderStats();
            EXPECT_EQ(allStreamsVideoStats.size(), 2);
            for (const auto& videoStats : allStreamsVideoStats)
            {
                const double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
                if (id == 0)
                {
                    EXPECT_NEAR(fps, 30.0, 5.0);
                    EXPECT_NEAR(videoStats.numDecodedFrames, 146, 20);
                }
                else
                {
                    EXPECT_NEAR(fps, 30.0, 2.0);
                    EXPECT_NEAR(videoStats.numDecodedFrames, 150, 20);
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
            *_publicTransportFactory,
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
        CallConfigBuilder cfg1(conf.getId());
        cfg1.av().url(baseUrl);

        CallConfigBuilder cfg2(cfg1);
        cfg2.room(conf2.getId()).url(baseUrl2);

        group.clients[0]->initiateCall(cfg1.build());
        group.clients[1]->joinCall(cfg2.build());
        group.clients[2]->joinCall(cfg2.build());

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

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

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
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }
        }

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary1;
        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
        auto videoReceiveStats = group.clients[0]->getCumulativeVideoReceiveCounters();
        group.clients[1]->getReportSummary(transportSummary1);
        group.clients[2]->getReportSummary(transportSummary2);

        logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
        logVideoReceive("client1", *group.clients[0]);
        logTransportSummary("client2", transportSummary1);
        logTransportSummary("client3", transportSummary2);

        auto allStreamsVideoStats = group.clients[0]->getActiveVideoDecoderStats();
        EXPECT_EQ(allStreamsVideoStats.size(), 2);
        for (const auto& videoStats : allStreamsVideoStats)
        {
            const double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
            EXPECT_NEAR(fps, 30.0, 1.0);
            EXPECT_NEAR(videoStats.numDecodedFrames, 150, 7);
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
            *_publicTransportFactory,
            *_sslDtls,
            1);
        group.add(&httpd2);

        Conference conf(_httpd);
        Conference conf2(&httpd2);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        CallConfigBuilder cfg1(conf.getId());
        cfg1.av().url(baseUrl);

        CallConfigBuilder cfg2(conf2.getId());
        cfg2.av().url(baseUrl2);

        group.clients[0]->initiateCall(cfg1.build());
        group.clients[1]->joinCall(cfg2.build());

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
        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);

        const double expectedFrequencies[2] = {1300.0, 600.0};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);

            ASSERT_GE(data.dominantFrequencies.size(), 1);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId++], 25.0);

            std::string clientName = "Client-" + std::to_string(id + 1);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            auto videoReceiveStats = group.clients[id]->getCumulativeVideoReceiveCounters();
            group.clients[id]->getReportSummary(transportSummary);

            logger::debug("%s received video pkts %" PRIu64, "bbTest", clientName.c_str(), videoReceiveStats.packets);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
            logTransportSummary(clientName.c_str(), transportSummary);

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
            *_publicTransportFactory,
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

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo();

        CallConfigBuilder cfgNeighbours(cfgBuilder);
        cfgNeighbours.neighbours(utils::Span<std::string>(neighbours));

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgNeighbours.build());
        group.clients[2]->joinCall(cfgNeighbours.url(baseUrl2).room(conf2.getId()).build());

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

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const size_t expectedFreqCount[] = {2, 1, 1};
        for (size_t id = 0; id < 3; ++id)
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), expectedFreqCount[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    });
}

TEST_F(BarbellTest, barbellStats)
{
#ifdef TCHECK_BUILD
    enterRealTime(2 + _numWorkerThreads);
    GTEST_SKIP();
#endif
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
            *_publicTransportFactory,
            *_sslDtls,
            1);
        group.add(&httpd2);
        group.add(&httpd2);

        Conference conf(_httpd);
        Conference conf2(&httpd2);

        const std::string barbellId = "test_barbell";
        Barbell bb1(_httpd, barbellId);
        Barbell bb2(&httpd2, barbellId);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);
        CallConfigBuilder cfg1(conf.getId());
        cfg1.av().url(baseUrl);

        CallConfigBuilder cfg2(cfg1);
        cfg2.room(conf2.getId()).url(baseUrl2);

        group.clients[0]->initiateCall(cfg1.build());
        group.clients[1]->joinCall(cfg2.build());
        group.clients[2]->joinCall(cfg2.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        makeShortCallWithDefaultAudioProfile(group, utils::Time::sec * 10);

        // Let's check both form of getting barbell stats:
        // - specifc conference: /stats/barbell/<conference ID>
        // - full (all conferences): /stats/barbell
        // Since both sides of the barbell have only one conference, we can get SPECIFIC stats from one side,
        // all (one) conference's stats from another and compare for symmetry.

        // Gather SPECIFIC conference's barbell stats from BB 1 (there is only one conference):
        nlohmann::json statsResponseBody1;
        std::string statsRequestUrl = std::string(baseUrl) + "/stats/barbell/" + conf.getId();
        auto barbellStatsRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            statsRequestUrl,
            1500 * utils::Time::ms,
            statsResponseBody1);
        EXPECT_TRUE(barbellStatsRequest);

        logger::debug("%s %s", "bbTest", statsRequestUrl.c_str(), statsResponseBody1.dump(4).c_str());

        // Gather ALL barbell stats from BB 2:
        nlohmann::json statsResponseBody2;
        statsRequestUrl = std::string(baseUrl2) + "/stats/barbell";
        barbellStatsRequest = emulator::awaitResponse<HttpGetRequest>(&httpd2,
            statsRequestUrl,
            1500 * utils::Time::ms,
            statsResponseBody2);
        EXPECT_TRUE(barbellStatsRequest);

        logger::debug("%s %s", "bbTest", statsRequestUrl.c_str(), statsResponseBody2.dump(4).c_str());

        // Main checks:
        // - symmetry of bitrates/packets/active streams for BB's inbound/outbound.
        // - matching payload characteristics, e.g. for audio, Opus packet rate (20ms --> 50 packets per second)
        // and for video bitrate of about 1 Mbps per active stream.

        {
            EXPECT_TRUE(statsResponseBody1.begin() != statsResponseBody1.end());
            EXPECT_EQ(statsResponseBody1.begin().key(), conf.getId());

            EXPECT_TRUE(statsResponseBody2.begin() != statsResponseBody2.end());
            EXPECT_EQ(statsResponseBody2.begin().key(), conf2.getId());

            auto bb1Stats = statsResponseBody1[cfg1.build().conferenceId];
            auto bb2Stats = statsResponseBody2[cfg2.build().conferenceId];
            ASSERT_TRUE(bb1Stats != nullptr);
            ASSERT_TRUE(bb2Stats != nullptr);
            auto s1 = bb1Stats[barbellId];
            auto s2 = bb2Stats[barbellId];
            ASSERT_TRUE(s1 != nullptr);
            ASSERT_TRUE(s2 != nullptr);

            // AUDIO:

            //  Active stream count symmetry (hardcoded expectations due to how we set up the test:
            // 2 users on one side of the barbell, and 1 on another).
            EXPECT_EQ(s1["audio"]["inbound"]["activeStreamCount"], 2);
            EXPECT_EQ(s2["audio"]["outbound"]["activeStreamCount"], 2);

            EXPECT_EQ(s1["audio"]["outbound"]["activeStreamCount"], 1);
            EXPECT_EQ(s2["audio"]["inbound"]["activeStreamCount"], 1);

            // Packates per second for audio (expectations for values per second we can hardcode):
            EXPECT_NEAR(s1["audio"]["inbound"]["packetsPerSecond"], 100, 5.0);
            EXPECT_NEAR(s2["audio"]["outbound"]["packetsPerSecond"], 100, 5.0);

            EXPECT_NEAR(s1["audio"]["outbound"]["packetsPerSecond"], 50, 3.0);
            EXPECT_NEAR(s2["audio"]["inbound"]["packetsPerSecond"], 50, 3.0);

            // Audio bitrate symmetry (expectations for values per second we can hardcode):
            EXPECT_NEAR(s1["audio"]["inbound"]["bitrateKbps"], 220, 10.0);
            EXPECT_NEAR(s2["audio"]["outbound"]["bitrateKbps"], 220, 10.0);

            EXPECT_NEAR(s1["audio"]["outbound"]["bitrateKbps"], 110, 5.0);
            EXPECT_NEAR(s2["audio"]["inbound"]["bitrateKbps"], 110, 5.0);

            // Packets sent / received symmetry (exact value could vary, but s1.inbound ~=~ s2.outbount):
            EXPECT_NE(s1["audio"]["inbound"]["packets"], 0);
            EXPECT_NE(s1["audio"]["outbound"]["packets"], 0);

            double packetsCompareInPercent = 100.0 *
                fabs((float)s1["audio"]["inbound"]["packets"] - (float)s2["audio"]["outbound"]["packets"]) /
                (float)s1["audio"]["inbound"]["packets"];
            EXPECT_NEAR(packetsCompareInPercent, 0.0, 5.0);

            // Octets sent / received symmetry (exact value could vary, but s1.inbound ~=~ s2.outbount):
            EXPECT_NE(s1["audio"]["inbound"]["octets"], 0);
            EXPECT_NE(s1["audio"]["outbound"]["octets"], 0);

            double octetsCompareInPercent = 100.0 *
                fabs((float)s1["audio"]["inbound"]["octets"] - (float)s2["audio"]["outbound"]["octets"]) /
                (float)s1["audio"]["inbound"]["octets"];
            EXPECT_NEAR(octetsCompareInPercent, 0.0, 5.0);

            // VIDEO:

            //  Active stream count symmetry (hardcoded expectations due to how we set up the test:
            // 2 users on one side of the barbell, and 1 on another).
            EXPECT_EQ(s1["video"]["inbound"]["activeStreamCount"], 6);
            EXPECT_EQ(s2["video"]["outbound"]["activeStreamCount"], 6);

            EXPECT_EQ(s1["video"]["outbound"]["activeStreamCount"], 3);
            EXPECT_EQ(s2["video"]["inbound"]["activeStreamCount"], 3);

            // Packates per second for audio (expectations for values per second we can hardcode):
            EXPECT_NEAR(s1["video"]["inbound"]["packetsPerSecond"], 500, 30.0);
            EXPECT_NEAR(s2["video"]["outbound"]["packetsPerSecond"], 500, 30.0);

            EXPECT_NEAR(s1["video"]["outbound"]["packetsPerSecond"], 250, 15.0);
            EXPECT_NEAR(s2["video"]["inbound"]["packetsPerSecond"], 250, 15.0);

            // Audio bitrate symmetry (expectations for values per second we can hardcode):
            EXPECT_NEAR(s1["video"]["inbound"]["bitrateKbps"], 4500, 200.0);
            EXPECT_NEAR(s2["video"]["outbound"]["bitrateKbps"], 4500, 200.0);

            EXPECT_NEAR(s1["video"]["outbound"]["bitrateKbps"], 2235, 100.0);
            EXPECT_NEAR(s2["video"]["inbound"]["bitrateKbps"], 2235, 100.0);

            // Packets sent / received symmetry (exact value could vary, but s1.inbound ~=~ s2.outbount):
            EXPECT_NE(s1["video"]["inbound"]["packets"], 0);
            EXPECT_NE(s1["video"]["outbound"]["packets"], 0);

            packetsCompareInPercent = 100.0 *
                fabs((float)s1["video"]["inbound"]["packets"] - (float)s2["video"]["outbound"]["packets"]) /
                (float)s1["video"]["inbound"]["packets"];
            EXPECT_NEAR(packetsCompareInPercent, 0.0, 8.0);

            // Octets sent / received symmetry (exact value could vary, but s1.inbound ~=~ s2.outbount):
            EXPECT_NE(s1["video"]["inbound"]["octets"], 0);
            EXPECT_NE(s1["video"]["outbound"]["octets"], 0);

            octetsCompareInPercent = 100.0 *
                fabs((float)s1["video"]["inbound"]["octets"] - (float)s2["video"]["outbound"]["octets"]) /
                (float)s1["video"]["inbound"]["octets"];
            EXPECT_NEAR(octetsCompareInPercent, 0.0, 8.0);
        }

        bb1.remove(baseUrl);

        utils::Time::nanoSleep(utils::Time::ms * 1000); // let pending packets be sent and received)

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);
        logVideoSent("client3", *group.clients[2]);

        const double expectedFrequencies[2][2] = {{1300.0, 2100.0}, {600.0, 2100.0}};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 10, true);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }
        }

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary1;
        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
        auto videoReceiveStats = group.clients[0]->getCumulativeVideoReceiveCounters();
        group.clients[1]->getReportSummary(transportSummary1);
        group.clients[2]->getReportSummary(transportSummary2);

        logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
        logVideoReceive("client1", *group.clients[0]);
        logTransportSummary("client2", transportSummary1);
        logTransportSummary("client3", transportSummary2);

        auto allStreamsVideoStats = group.clients[0]->getActiveVideoDecoderStats();
        EXPECT_EQ(allStreamsVideoStats.size(), 2);
        for (const auto& videoStats : allStreamsVideoStats)
        {
            const double fps = (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
            EXPECT_NEAR(fps, 30.0, 1.0);
            EXPECT_NEAR(videoStats.numDecodedFrames, 300, 15);
        }
    });
}
