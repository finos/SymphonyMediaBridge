#include "test/integration/IntegrationTest.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/HttpRequests.h"
#include "utils/Format.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

class IntegrationSrtpTest : public IntegrationTest, public ::testing::WithParamInterface<srtp::Profile>
{
};

using namespace emulator;

TEST_P(IntegrationSrtpTest, oneOnOneSDES)
{
    auto srtpProfile = GetParam();
    runTestInThread(expectedTestThreadCount(1), [this, srtpProfile]() {
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
            *_publicTransportFactory,
            *_sslDtls,
            2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo().sdes(srtpProfile).disableDtls();

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgBuilder.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        makeShortCallWithDefaultAudioProfile(group, utils::Time::sec * 2);

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

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[2][1] = {{1300.0}, {600.0}};
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 2, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[id][0], 25.0);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    });
}

INSTANTIATE_TEST_SUITE_P(IntegrationSrtpTest,
    IntegrationSrtpTest,
    testing::Values(srtp::Profile::AES128_CM_SHA1_32,
        srtp::Profile::AES128_CM_SHA1_80,
        srtp::Profile::AES_256_CM_SHA1_80,
        srtp::Profile::AES_256_CM_SHA1_32,
        srtp::Profile::AES_192_CM_SHA1_80,
        srtp::Profile::AES_192_CM_SHA1_32,
        srtp::Profile::AEAD_AES_128_GCM,
        srtp::Profile::AEAD_AES_256_GCM));

TEST_F(IntegrationTest, nullCipherNotAllowed)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_publicTransportFactory,
            *_sslDtls,
            1);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av().disableDtls();

        group.clients[0]->initiateCall(cfg.build());

        ASSERT_FALSE(group.connectAll(utils::Time::sec * 1));

        EXPECT_FALSE(group.clients[0]->isEndpointConfigured());
    });
}
