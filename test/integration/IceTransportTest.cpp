#include "test/integration/IntegrationTest.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/HttpRequests.h"
#include "utils/Format.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

class IceTransportEmuTest : public IntegrationTest
{
};

using namespace emulator;

TEST_F(IceTransportEmuTest, plainNewApi)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);
        _config.ice.tcp.enable = true;
        _clientConfig.ice.tcp.enable = true;

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            3);

        for (auto pairIt : this->_endpointNetworkLinkMap)
        {
            if (utils::startsWith("FakeUdp", pairIt.first))
            {
                pairIt.second.ptrLink->setStaticDelay(pairIt.second.address.getFamily() == AF_INET6 ? 120 : 15);
            }
        }

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto confRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(confRequest);

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
            EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);

            if (2 == id)
            {
                EXPECT_GE(data.amplitudeProfile.size(), 2);
                for (auto& item : data.amplitudeProfile)
                {
                    logger::debug("%.3fs, %.3f", "", item.first / 48000.0, item.second);
                }
                // We expect a ramp-up of volume like this:
                // start from 0;
                // ramp-up to about 1826 (+-250) in 0.8 (+-0,2s)
                if (data.amplitudeProfile.size() >= 2)
                {
                    EXPECT_EQ(data.amplitudeProfile[0].second, 0);

                    EXPECT_NEAR(data.amplitudeProfile.back().second, 1826, 250);
                    EXPECT_NEAR(data.amplitudeProfile.back().first, 48000 * 0.79, 48000 * 0.2);
                }

                EXPECT_EQ(data.audioSsrcCount, 1);
            }

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->_transport->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    });
}

/*
    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewallLocal(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewallVpn(transport::SocketAddress::parse("2001:1ba8:148c:bf00:9181:9fb2:8def:a66e", 0),
        internet);

    fakenet::FakeEndpoint endpoint1(transport::SocketAddress::parse("192.168.1.10", 2000), firewallLocal);
    fakenet::FakeEndpoint endpointTcp1(transport::SocketAddress::parse("192.168.1.10", 6700), firewallLocal);
    fakenet::FakeEndpoint endpoint2(transport::SocketAddress::parse("fc00:1808:1808:1808:1808:1808:1808:1100", 3000),
        firewallVpn);

    fakenet::FakeEndpoint smbEndpoint(transport::SocketAddress::parse("111.11.1.11", 10000), internet);
    fakenet::FakeEndpoint smbEndpointIpv6(transport::SocketAddress::parse("2600:1900:4080:1627:0:26:0:0", 10000),
        internet);
    fakenet::FakeEndpoint smbTcpServerEndpoint(transport::SocketAddress::parse("111.11.1.11", 4443),
        internet,
        ice::TransportType::TCP);
*/
