#include "test/integration/IntegrationTest.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/HttpRequests.h"
#include "utils/Format.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

class SrtpTransportEmuTest : public IntegrationTest
{
};

using namespace emulator;

TEST_F(SrtpTransportEmuTest, oneOnOneSDES)
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
            2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo().sdes().disableDtls();

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgBuilder.build());

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

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[2][1] = {{1300.0}, {600.0}};
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[id][0], 25.0);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->_transport->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    });
}
