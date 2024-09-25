#include "api/Parser.h"
#include "api/utils.h"
#include "bridge/Mixer.h"
#include "bridge/endpointActions/ApiHelpers.h"
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
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/HttpRequests.h"
#include "transport/DataReceiver.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/SimpleJson.h"
#include "utils/StringBuilder.h"
#include <chrono>
#include <complex>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <unordered_set>

using namespace emulator;

enum class EncryptionMode
{
    DTLS = 1,
    SDES,
    NullCipher
};

class IntegrationCallTypesTest : public IntegrationTest,
                                 public ::testing::WithParamInterface<std::tuple<TransportMode, EncryptionMode>>
{
    void SetUp() override
    {
#ifdef NOPERF_TEST
        GTEST_SKIP();
#endif
        IntegrationTest::SetUp();
    }

    void TearDown() override
    {
#ifdef NOPERF_TEST
        GTEST_SKIP();
#endif
        IntegrationTest::TearDown();
    }
};

TEST_P(IntegrationCallTypesTest, party3AllModes)
{
    auto transportMode = std::get<0>(GetParam());
    auto encryptionMode = std::get<1>(GetParam());

    logger::info("test transportmode %d, encryption %d", "party3AllModes", static_cast<int>(transportMode), static_cast<int>(encryptionMode));
    auto testBody = [this, transportMode, encryptionMode]() {
        _config.readFromString(_defaultSmbConfig);
        _config.enableSrtpNullCipher = true;

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_publicTransportFactory,
            *_sslDtls,
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();
        switch (transportMode)
        {
        case TransportMode::StreamTransportIce:
            cfg.streamTransportIce();
            break;
        case TransportMode::StreamTransportNoIce:
            cfg.streamTransportNoIce();
            break;
        default:
            break;
        }

        switch (encryptionMode)
        {
        case EncryptionMode::SDES:
            cfg.sdes();
            cfg.disableDtls();
            break;
        case EncryptionMode::NullCipher:
            cfg.sdes(srtp::Profile::NULL_CIPHER);
            cfg.disableDtls();
            break;
        default:
            break;
        }

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.forward().build());
        group.clients[2]->joinCall(cfg.mixed().build());

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

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        if (encryptionMode != EncryptionMode::DTLS)
        {
            finalizeSimulation();
        }

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data =
                analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0, false);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId][1], 25.0);
            }
            ++freqId;
            if (2 == id)
            {
                EXPECT_GE(data.amplitudeProfile.size(), 2);
                for (auto& item : data.amplitudeProfile)
                {
                    logger::debug("%.3fs, %.3f", "", item.first / 48000.0, item.second);
                }
                // We expect a ramp-up of volume like this:
                // start from 0;
                // ramp-up to about 3652 (+-250) in 0.8 (+-0,2s)
                if (data.amplitudeProfile.size() >= 2)
                {
                    EXPECT_LT(data.amplitudeProfile[0].second, 100);

                    EXPECT_NEAR(std::max_element(data.amplitudeProfile.begin(), data.amplitudeProfile.end())->second,
                        3600,
                        550);
                    EXPECT_NEAR(data.rampupAbove(3100), 48000 * 1.22, 48000);
                }

                EXPECT_EQ(data.audioSsrcCount, 1);
            }

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }
    };

    if (encryptionMode != EncryptionMode::DTLS)
    {
        runTestInThread(expectedTestThreadCount(1), testBody);
    }
    else
    {
        enterRealTime(2 + _numWorkerThreads);
        logger::info("running test in real time to avoid DTLS problems", "party3AllModes");
        testBody();
    }
}

INSTANTIATE_TEST_SUITE_P(IntegrationCallTypesTest,
    IntegrationCallTypesTest,
    testing::Combine(testing::Values(TransportMode::BundledIce,
                         TransportMode::StreamTransportIce,
                         TransportMode::StreamTransportNoIce),
        testing::Values(EncryptionMode::DTLS, EncryptionMode::SDES, EncryptionMode::NullCipher)));
