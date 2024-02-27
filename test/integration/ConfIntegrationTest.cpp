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
const uint32_t MIXED_VOLUME = 3652;

TEST_F(IntegrationTest, plain)
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
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
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

        auto aboutVersionSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/about/version",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(aboutVersionSuccess);

        auto aboutHealthSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/about/health",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(aboutHealthSuccess);

        auto aboutCapabilitiesSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/about/capabilities",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(aboutCapabilitiesSuccess);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() < 2)
            {
                continue;
            }
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
                // ramp-up to about 3652 (+-250) in 0.8 (+-0,2s)
                if (data.amplitudeProfile.size() >= 2)
                {
                    EXPECT_LT(data.amplitudeProfile[0].second, 100);

                    EXPECT_NEAR(data.amplitudeProfile.back().second, MIXED_VOLUME, 750);
                    EXPECT_NEAR(data.rampupAbove(3100), 48000 * 1.25, 48000 * 0.2);
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
    });
}

TEST_F(IntegrationTest, ptime10)
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
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av().ptime(10);

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
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
        finalizeSimulation();

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
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }

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

                    EXPECT_NEAR(data.amplitudeProfile.back().second, MIXED_VOLUME, 750);
                    EXPECT_NEAR(data.rampupAbove(3100), 48000 * 0.74, 48000 * 0.2);
                }

                EXPECT_EQ(data.audioSsrcCount, 1);
            }
        }
    });
}

TEST_F(IntegrationTest, detectIsPtt)
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
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulationWithTimeout, this, utils::Time::sec));
        startSimulation();

        group.startConference(conf, baseUrl);

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        group.clients[2]->joinCall(cfg.build());

        auto connectResult = group.connectAll(utils::Time::sec * _clientsConnectionTimeout);
        ASSERT_TRUE(connectResult);
        if (!connectResult)
        {
            return;
        }

        group.clients[0]->_audioSource->setFrequency(600);
        group.clients[1]->_audioSource->setFrequency(1300);
        group.clients[2]->_audioSource->setFrequency(2100);

        group.clients[0]->_audioSource->setVolume(0.001);
        group.clients[1]->_audioSource->setVolume(0.001);
        group.clients[2]->_audioSource->setVolume(0.001);

        // Disable audio level extension, otherwise constant signal will lead to the 'noise leve' equal to the
        // signal and detection would fail
        group.clients[0]->_audioSource->setUseAudioLevel(false);
        group.clients[1]->_audioSource->setUseAudioLevel(false);
        group.clients[2]->_audioSource->setUseAudioLevel(false);

        group.run(utils::Time::ms * 300);

        // =============================== PART 1: #1 & #2 talking ====================

        group.clients[0]->_audioSource->setPtt(emulator::AudioSource::PttState::Set);
        group.clients[1]->_audioSource->setPtt(emulator::AudioSource::PttState::Set);
        group.clients[0]->_audioSource->setVolume(0.8);
        group.clients[1]->_audioSource->setVolume(0.8);
        group.clients[2]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[2]->_audioSource->setVolume(0.0);

        logger::info("Send on user 0, 1", "Test");
        group.run(utils::Time::sec * 2);

        auto endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        auto endpointExtendedInfo = getEndpointExtendedInfo(_httpd, baseUrl, endpoints[0].id);

        EXPECT_EQ(endpoints[0], endpointExtendedInfo.basicEndpointInfo);
        EXPECT_EQ(10000, endpointExtendedInfo.localPort);

        // We construct pseudo-usid from ssrc, so we can check it here.
        auto expectedUsid = __builtin_bswap32(endpointExtendedInfo.ssrcOriginal << 8);
        EXPECT_TRUE(1 << 24 > endpointExtendedInfo.userId.get());
        EXPECT_EQ(expectedUsid, endpointExtendedInfo.userId.get());
        EXPECT_EQ(endpointExtendedInfo.ssrcOriginal, group.clients[0]->_audioSource->getSsrc());
        EXPECT_NE(endpointExtendedInfo.ssrcOriginal, endpointExtendedInfo.ssrcRewritten);
        EXPECT_TRUE(endpointExtendedInfo.ssrcRewritten != 0);

        // =============================== PART 2: #2 talking =========================

        group.clients[0]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[0]->_audioSource->setVolume(0.0);
        group.clients[1]->_audioSource->setPtt(emulator::AudioSource::PttState::Set);
        group.clients[1]->_audioSource->setVolume(0.8);
        group.clients[2]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[2]->_audioSource->setVolume(0.0);
        logger::info("Send on user 1", "Test");

        group.run(utils::Time::sec * 2);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        utils::Time::nanoSleep(utils::Time::sec * 1);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 3: #3 talking =========================

        group.clients[0]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[0]->_audioSource->setVolume(0.0);
        group.clients[1]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[1]->_audioSource->setVolume(0.0);
        group.clients[2]->_audioSource->setPtt(emulator::AudioSource::PttState::Set);
        group.clients[2]->_audioSource->setVolume(0.8);
        logger::info("Send on user 2", "Test");

        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 4: nobody talking =====================

        group.clients[0]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[1]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[2]->_audioSource->setPtt(emulator::AudioSource::PttState::Unset);
        group.clients[0]->_audioSource->setVolume(0.0);
        group.clients[1]->_audioSource->setVolume(0.0);
        group.clients[2]->_audioSource->setVolume(0.0);
        logger::info("Send on none", "Test");
        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        group.clients[2]->stopRecording();
        group.clients[1]->stopRecording();
        group.clients[0]->stopRecording();

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();
    });
};

TEST_F(IntegrationTest, endpointAutoRemove)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_publicTransportFactory,
            *_sslDtls,
            3);

        Conference conf(_httpd);
        group.startConference(conf, baseUrl);

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo();

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgBuilder.idleTimeout(10).build());
        group.clients[2]->joinCall(cfgBuilder.idleTimeout(10).build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(3, endpoints.size());

        logger::info("stopping endpoint 2", "Test");
        group.clients[2]->stopTransports();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(2, endpoints.size());

        group.clients[1]->stopTransports();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(1, endpoints.size());

        group.clients[0]->stopTransports();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(1, endpoints.size());

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();
    });
}

TEST_F(IntegrationTest, probing)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        // Retrieve probing ICE candidates
        const std::string baseUrl = "http://127.0.0.1:8080";

        nlohmann::json responseBody;
        auto candidatesSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            baseUrl + "/ice-candidates",
            5 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(candidatesSuccess);

        const auto& iceJson = responseBody["ice"];

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();
        logger::debug("%s", "", iceJson.dump(4).c_str());

        api::Ice ice = api::Parser::parseIce(iceJson);
        auto candidatesAndCredentials = bridge::getIceCandidatesAndCredentials(ice);

        // Setup transport and attempt to connect to trigger ICE probing
        // Note: use CONTROLLING role
        auto transport = _clientTransportFactory->createOnPrivatePort(ice::IceRole::CONTROLLING, 256 * 1024, 1);

        transport->setRemoteIce(candidatesAndCredentials.second, candidatesAndCredentials.first, _audioAllocator);
        transport->start();

        ASSERT_EQ(transport->getRtt(), 0);

        transport->connect();

        auto timeout = utils::Time::ms * 2000;
        auto start = utils::Time::getAbsoluteTime();
        while (transport->hasPendingJobs() && utils::Time::getAbsoluteTime() - start < timeout)
        {
            utils::Time::nanoSleep(utils::Time::ms * 100);
        }

        // non-zero RTT indicates that there was a successful candidates pair, hence probing was performed
        ASSERT_GT(transport->getRtt(), 0);
        transport->stop();
        while (transport->hasPendingJobs() && utils::Time::getAbsoluteTime() - start < timeout)
        {
            utils::Time::nanoSleep(utils::Time::ms * 100);
        }
        finalizeSimulation();
    });
}

TEST_F(IntegrationTest, conferencePort)
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
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl, false);

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        group.clients[2]->joinCall(cfg.mixed().build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        const auto smbPort = group.clients[0]->_bundleTransport->getRemotePeer();
        EXPECT_NE(smbPort.getPort(), 10000);
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
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }

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

                    EXPECT_NEAR(data.amplitudeProfile.back().second, MIXED_VOLUME, 750);
                    EXPECT_NEAR(data.rampupAbove(3100), 48000 * 1.25, 48000 * 0.2);
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
    });
}

TEST_F(IntegrationTest, neighbours)
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
            4);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        std::string neighbours[] = {"gid1"};

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo();

        CallConfigBuilder cfgNeighbours(cfgBuilder);
        cfgNeighbours.neighbours(utils::Span<std::string>(neighbours));

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgNeighbours.build());
        group.clients[2]->joinCall(cfgNeighbours.build());
        group.clients[3]->joinCall(cfgNeighbours.mixed().build());
        // 600, 1300, 2100, 3200
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

        group.stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const size_t chMixed[] = {0, 0, 0, 1};
        AudioAnalysisData results[4];
        for (size_t id = 0; id < 4; ++id)
        {
            results[id] = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, chMixed[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }

        EXPECT_EQ(results[0].audioSsrcCount, 3u);
        EXPECT_EQ(results[1].audioSsrcCount, 1u);
        EXPECT_EQ(results[2].audioSsrcCount, 1u);
        EXPECT_EQ(results[3].audioSsrcCount, 1u);

        EXPECT_EQ(results[0].dominantFrequencies.size(), 3);
        EXPECT_EQ(results[1].dominantFrequencies.size(), 1);
        EXPECT_EQ(results[2].dominantFrequencies.size(), 1);
        EXPECT_EQ(results[3].dominantFrequencies.size(), 1);

        if (results[3].dominantFrequencies.size() > 0)
        {
            EXPECT_NEAR(results[3].dominantFrequencies[0], 600.0, 50.0);
        }
    });
}

TEST_F(IntegrationTest, dynamicNeighbours_removeNeighbours)
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
            4);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        std::string neighbours[] = {"gid1"};

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo();

        CallConfigBuilder cfgNeighbours(cfgBuilder);
        cfgNeighbours.neighbours(utils::Span<std::string>(neighbours));

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgNeighbours.build());
        group.clients[2]->joinCall(cfgNeighbours.build());
        group.clients[3]->joinCall(cfgNeighbours.mixed().build());

        // 600, 1300, 2100, 3200
        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        nlohmann::json responseBody;
        const auto conferenceId = conf.getId();

        for (int i = 1; i < 4; i++)
        {
            auto endpointId = group.clients[i]->getEndpointId();

            nlohmann::json body = {{"action", "reconfigure"}};
            body["neighbours"] = {{"groups", nlohmann::json::array()}};

            auto neighboursSet = emulator::awaitResponse<HttpPutRequest>(_httpd,
                std::string(baseUrl) + "/conferences/" + conferenceId + "/" + endpointId,
                body.dump(),
                1.5 * utils::Time::sec,
                responseBody);
            EXPECT_TRUE(neighboursSet);
        }

        make5secCallWithDefaultAudioProfile(group);

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

        group.stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const size_t chMixed[] = {0, 0, 0, 3};
        AudioAnalysisData results[4];
        for (size_t id = 0; id < 4; ++id)
        {
            results[id] = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, chMixed[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }

        EXPECT_EQ(results[0].audioSsrcCount, 3u);
        EXPECT_EQ(results[1].audioSsrcCount, 3u);
        EXPECT_EQ(results[2].audioSsrcCount, 3u);
        EXPECT_EQ(results[3].audioSsrcCount, 1u);

        EXPECT_EQ(results[0].dominantFrequencies.size(), 3);
        EXPECT_EQ(results[1].dominantFrequencies.size(), 3);
        EXPECT_EQ(results[2].dominantFrequencies.size(), 3);
        EXPECT_EQ(results[3].dominantFrequencies.size(), 3);

        if (results[3].dominantFrequencies.size() > 0)
        {
            EXPECT_NEAR(results[3].dominantFrequencies[0], 600.0, 50.0);
        }
    });
}

TEST_F(IntegrationTest, dynamicNeighbours_addNeighbours)
{

// TODO: this testing if failing on jenkins with LCHECK_BUILD, it seems to be a test problem (not production code)
#ifdef LCHECK_BUILD
    enterRealTime(2 + _numWorkerThreads);
    GTEST_SKIP();
#endif
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
            4);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        std::string neighbourGroupName = "gid1";
        auto neighbourGroups = nlohmann::json::array();
        neighbourGroups.push_back(neighbourGroupName);

        CallConfigBuilder cfgBuilder(conf.getId());
        cfgBuilder.url(baseUrl).withOpus().withVideo();

        group.clients[0]->initiateCall(cfgBuilder.build());
        group.clients[1]->joinCall(cfgBuilder.build());
        group.clients[2]->joinCall(cfgBuilder.build());
        group.clients[3]->joinCall(cfgBuilder.mixed().build());

        // 600, 1300, 2100, 3200
        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        nlohmann::json responseBody;
        const auto conferenceId = conf.getId();

        for (int i = 1; i < 4; i++)
        {
            auto endpointId = group.clients[i]->getEndpointId();

            nlohmann::json body = {{"action", "reconfigure"}};
            body["neighbours"] = {{"groups", neighbourGroups}};

            auto neighboursSet = emulator::awaitResponse<HttpPutRequest>(_httpd,
                std::string(baseUrl) + "/conferences/" + conferenceId + "/" + endpointId,
                body.dump(),
                1.5 * utils::Time::sec,
                responseBody);
            EXPECT_TRUE(neighboursSet);
        }

        make5secCallWithDefaultAudioProfile(group);

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

        group.stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const size_t chMixed[] = {0, 0, 0, 1};
        AudioAnalysisData results[4];
        for (size_t id = 0; id < 4; ++id)
        {
            results[id] = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, chMixed[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), transportSummary);

            logVideoSent(clientName.c_str(), *group.clients[id]);
            logVideoReceive(clientName.c_str(), *group.clients[id]);
        }

        EXPECT_EQ(results[0].audioSsrcCount, 3u);
        EXPECT_EQ(results[1].audioSsrcCount, 1u);
        EXPECT_EQ(results[2].audioSsrcCount, 1u);
        EXPECT_EQ(results[3].audioSsrcCount, 1u);

        EXPECT_EQ(results[0].dominantFrequencies.size(), 3);
        EXPECT_EQ(results[1].dominantFrequencies.size(), 1);
        EXPECT_EQ(results[2].dominantFrequencies.size(), 1);
        EXPECT_EQ(results[3].dominantFrequencies.size(), 1);

        if (results[3].dominantFrequencies.size() > 0)
        {
            EXPECT_NEAR(results[3].dominantFrequencies[0], 600.0, 50.0);
        }
    });
}

class WebRtcListenerMock : public webrtc::WebRtcDataStream::Listener
{
public:
    MOCK_METHOD(void, onWebRtcDataString, (const char* m, size_t len));
};

TEST_F(IntegrationTest, endpointMessage)
{
    using namespace testing;

    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        const char* message = R"({
        "payload":"Good",
        "type":"connectivity" 
        })";

        WebRtcListenerMock listenerMock;

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

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        static const double frequencies[] = {600, 1300};
        for (size_t i = 0; i < group.clients.size(); ++i)
        {
            group.clients[i]->_audioSource->setFrequency(frequencies[i]);
        }

        for (auto& client : group.clients)
        {
            client->_audioSource->setVolume(0.6);
        }

        group.clients[1]->setDataListener(&listenerMock);
        int endpointMessageCount = 0;
        ON_CALL(listenerMock, onWebRtcDataString).WillByDefault([&endpointMessageCount](const char* m, size_t len) {
            auto json = utils::SimpleJson::create(m, len);
            char typeName[100];
            logger::debug("recv data channel message %s", "Test", m);
            if (json["colibriClass"].getString(typeName) && std::strcmp(typeName, "EndpointMessage") == 0)
            {
                ++endpointMessageCount;
            }
        });
        // Dom speaker + UserMap + endpont message
        EXPECT_CALL(listenerMock, onWebRtcDataString(_, _)).Times(AtLeast(3));
        group.run(utils::Time::sec * 2);

        logger::info("sending endpoint message", "Test");
        group.clients[0]->sendEndpointMessage(group.clients[1]->getEndpointId(), message);

        group.run(utils::Time::sec * 2);
        logger::flushLog();
        EXPECT_EQ(endpointMessageCount, 1);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();
    });
}

TEST_F(IntegrationTest, noAudioLevelExt)
{
#ifdef NOPERF_TEST
    enterRealTime(2 + _numWorkerThreads);
    GTEST_SKIP();
#endif

    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);
        _config.audio.lastN = 1;

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

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
        cfg.url(baseUrl).withOpus();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        group.clients[2]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));
        group.clients[2]->_audioSource->setUseAudioLevel(false);

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);
        logger::info("%s", "Test", responseBody.dump(3).c_str());
        EXPECT_EQ(responseBody["inbound_audio_streams"].get<uint32_t>(), 3);
        EXPECT_EQ(responseBody["inbound_audio_ext_streams"].get<uint32_t>(), 2);
        EXPECT_NEAR(responseBody["opus_decode_packet_rate"].get<double>(), 50.0, 1.0);

        auto endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(3, endpoints.size());
        size_t dominantSpeakerCount = 0;
        for (const auto& endpoint : endpoints)
        {
            if (endpoint.isDominantSpeaker)
            {
                dominantSpeakerCount++;
            }
            EXPECT_TRUE(endpoint.dtlsState == transport::SrtpClient::State::CONNECTED);
            EXPECT_TRUE(endpoint.iceState == ice::IceSession::State::CONNECTED);
        }
        EXPECT_EQ(1, dominantSpeakerCount);

        group.stopTransports();
        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }
        }
    });
}

TEST_F(IntegrationTest, confList)
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
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
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

        auto briefConfRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences?brief=1",
            1500 * utils::Time::ms,
            responseBody);
        logger::info("%s", "test", responseBody.dump(3).c_str());
        EXPECT_TRUE(responseBody.size() == 1);
        EXPECT_TRUE(briefConfRequest);
        auto& mixerJson = responseBody[0];
        EXPECT_EQ(mixerJson["id"], conf.getId());
        EXPECT_EQ(mixerJson["usercount"], group.clients.size());

        for (auto& ep : mixerJson["users"])
        {
            auto id = ep.get<std::string>();
            EXPECT_NE(std::find_if(group.clients.begin(),
                          group.clients.end(),
                          [&id](const std::unique_ptr<SfuClient<Channel>>& p) { return p->getEndpointId() == id; }),
                group.clients.end());
        }

        briefConfRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences?brief",
            1500 * utils::Time::ms,
            responseBody);

        briefConfRequest = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/conferences?brief=1&bread=true&butter",
            1500 * utils::Time::ms,
            responseBody);

        logger::info("%s", "test", responseBody.dump(3).c_str());
        EXPECT_TRUE(briefConfRequest);
        EXPECT_TRUE(responseBody.size() == 1);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }

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

                    EXPECT_NEAR(data.amplitudeProfile.back().second, MIXED_VOLUME, 750);
                    EXPECT_NEAR(data.rampupAbove(3100), 48000 * 1.25, 48000 * 0.2);
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
    });
}

TEST_F(IntegrationTest, opusDecodeRate)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        _config.audio.lastN = 1;

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

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
        cfg.url(baseUrl).withOpus();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        group.clients[2]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));
        group.clients[0]->_audioSource->setUseAudioLevel(false);
        group.clients[1]->_audioSource->setUseAudioLevel(false);
        group.clients[2]->_audioSource->setUseAudioLevel(false);

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);
        logger::info("%s", "Test", responseBody.dump(3).c_str());
        EXPECT_EQ(responseBody["inbound_audio_streams"].get<uint32_t>(), 3);
        EXPECT_EQ(responseBody["inbound_audio_ext_streams"].get<uint32_t>(), 0);
        EXPECT_NEAR(responseBody["opus_decode_packet_rate"].get<double>(), 150.0, 3.0);
        group.clients[2]->disconnect();

        group.run(utils::Time::sec * 5);
        utils::Time::nanoSleep(utils::Time::sec * 1);

        statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);
        // optimization in 2 party call will avoid opus decoding
        EXPECT_EQ(responseBody["opus_decode_packet_rate"].get<double>(), 0.0);

        group.stopTransports();
        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, true, 0);
            EXPECT_EQ(data.dominantFrequencies.size(), 2);
            if (data.dominantFrequencies.size() >= 2)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId][0], 25.0);
                EXPECT_NEAR(data.dominantFrequencies[1], expectedFrequencies[freqId++][1], 25.0);
            }
        }
    });
}

TEST_F(IntegrationTest, twoClientsAudioOnly)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

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

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).withOpus();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

        auto endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(2, endpoints.size());
        size_t dominantSpeakerCount = 0;
        for (const auto& endpoint : endpoints)
        {
            if (endpoint.isDominantSpeaker)
            {
                dominantSpeakerCount++;
            }
            EXPECT_TRUE(endpoint.dtlsState == transport::SrtpClient::State::CONNECTED);
            EXPECT_TRUE(endpoint.iceState == ice::IceSession::State::CONNECTED);
        }
        EXPECT_EQ(1, dominantSpeakerCount);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[2] = {1300.0, 600.0};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_GE(data.amplitudeProfile.size(), 3);
            if (data.amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(data.amplitudeProfile.back().second, 5725, 700);
            }
            if (data.dominantFrequencies.size() >= 1)
            {
                EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId++], 25.0);
            }
        }
    });
}

TEST_F(IntegrationTest, audioOnlyNoPadding)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        printf("T%" PRIu64 " test running\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

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

        printf("T%" PRIu64 " starting conf\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        group.startConference(conf, baseUrl);

        printf("T%" PRIu64 " calling\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);

        CallConfigBuilder callConfig(conf.getId());
        callConfig.withOpus().mixed().url(baseUrl);

        // Audio only for all three participants.
        group.clients[0]->initiateCall(callConfig.build());
        group.clients[1]->joinCall(callConfig.build());
        group.clients[2]->joinCall(callConfig.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        printf("T%" PRIu64 " run for 5s\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        make5secCallWithDefaultAudioProfile(group);

        printf("T%" PRIu64 " stopping recs\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        group.clients[2]->stopRecording();
        group.clients[1]->stopRecording();
        group.clients[0]->stopRecording();

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = group.clients[0]->getAudioReceiveStats();
        const auto& rData2 = group.clients[1]->getAudioReceiveStats();
        const auto& rData3 = group.clients[2]->getAudioReceiveStats();
        // We expect only one ssrc (audio), since padding (that comes on video ssrc) is disabled for audio only
        // calls).
        EXPECT_EQ(rData1.size(), 1);
        EXPECT_EQ(rData2.size(), 1);
        EXPECT_EQ(rData3.size(), 1);
    });
}

TEST_F(IntegrationTest, paddingOffWhenRtxNotProvided)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);
        const std::string baseUrl = "http://127.0.0.1:8080";

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

        using RtpVideoReceiver = typename emulator::RtpVideoReceiver;

        emulator::CallConfigBuilder config(conf.getId());
        config.withOpus().withVideo().disableRtx().url(baseUrl);

        // Audio only for all three participants.
        group.clients[0]->initiateCall(config.build());
        group.clients[1]->joinCall(config.build());
        group.clients[2]->joinCall(config.enableRtx().build());

        auto connectResult = group.connectAll(utils::Time::sec * _clientsConnectionTimeout);
        EXPECT_TRUE(connectResult);
        if (!connectResult)
        {
            return;
        }

        make5secCallWithDefaultAudioProfile(group);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = group.clients[0]->getAudioReceiveStats();
        const auto& rData2 = group.clients[1]->getAudioReceiveStats();
        const auto& rData3 = group.clients[2]->getAudioReceiveStats();

        EXPECT_EQ(rData1.size(), 2); // s2's audio, s3's audio
        EXPECT_EQ(rData2.size(), 2); // s1's audio, s3's audio
        EXPECT_EQ(rData3.size(), 2); // s1's audio, s2's audio

        auto videoReceivers1 = group.clients[0]->collectReceiversWithPackets();
        auto videoReceivers2 = group.clients[1]->collectReceiversWithPackets();
        auto videoReceivers3 = group.clients[2]->collectReceiversWithPackets();

        ASSERT_EQ(videoReceivers1.size(), 2); // s2's video, s3's video
        ASSERT_EQ(videoReceivers2.size(), 2); // s1's video, s3's video
        ASSERT_EQ(videoReceivers3.size(), 3); // s1's video, s3's video + padding

        // Check if all others have received video content only
        for (auto* videoReceiver : videoReceivers1)
        {
            ASSERT_GT(videoReceiver->videoPacketCount, 0); // It should contain video
            ASSERT_EQ(videoReceiver->rtxPacketCount, 0); // It should NOT contain rtx
            ASSERT_EQ(videoReceiver->unknownPayloadPacketCount,
                0); // It should NOT video have unknown payload types
            ASSERT_EQ(videoReceiver->getContent(), RtpVideoReceiver::VideoContent::VIDEO);
        }

        for (auto* videoReceiver : videoReceivers2)
        {
            ASSERT_GT(videoReceiver->videoPacketCount, 0); // It should contain video
            ASSERT_EQ(videoReceiver->rtxPacketCount, 0); // It should NOT contain rtx
            ASSERT_EQ(videoReceiver->unknownPayloadPacketCount,
                0); // It should NOT video have unknown payload types
            ASSERT_EQ(videoReceiver->getContent(), RtpVideoReceiver::VideoContent::VIDEO);
        }

        RtpVideoReceiver* localVideoReceiver = nullptr;
        for (auto* videoReceiver : videoReceivers3)
        {
            if (videoReceiver->getContent() == RtpVideoReceiver::VideoContent::LOCAL)
            {
                localVideoReceiver = videoReceiver;
            }
            else
            {
                ASSERT_GT(videoReceiver->videoPacketCount, 0); // It should contain video
                ASSERT_EQ(videoReceiver->rtxPacketCount, 0); // It should NOT contain rtx
                ASSERT_EQ(videoReceiver->unknownPayloadPacketCount,
                    0); // It should NOT video have unknown payload types
            }
        }

        ASSERT_NE(localVideoReceiver, nullptr);
        ASSERT_GT(localVideoReceiver->rtxPacketCount, 0); // It should contain rtx
        ASSERT_EQ(localVideoReceiver->videoPacketCount, 0); // It should NOT video packets
        ASSERT_EQ(localVideoReceiver->unknownPayloadPacketCount,
            0); // It should NOT video have unknown payload types
    });
}

TEST_F(IntegrationTest, videoOffPaddingOff)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        /*
           Test checks that after video is off and cooldown interval passed, no padding will be sent for the
           call that became audio-only.
        */

        _config.readFromString(_defaultSmbConfig);
        _config.rctl.cooldownInterval = 1;

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

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

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        // Audio only for all three participants.
        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        // Client 3 will join after client 2, which produce video, will leave.

        if (!group.connectAll(utils::Time::sec * _clientsConnectionTimeout))
        {
            EXPECT_TRUE(false);
            return;
        }

        // Have to produce some audio volume above "silence threshold", otherwise audio packets
        // won't be forwarded by SFU.
        group.clients[0]->_audioSource->setFrequency(600);
        group.clients[1]->_audioSource->setFrequency(1300);

        group.clients[0]->_audioSource->setVolume(0.6);
        group.clients[1]->_audioSource->setVolume(0.6);

        for (int i = 0; i < 100; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            group.clients[0]->process(timestamp, false);
            group.clients[1]->process(timestamp, true);
            _pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        group.clients[1]->stopRecording();
        group.clients[1]->stopTransports();

        // Video producer (client2) stopped, waiting 1.5s for rctl.cooldownInterval timeout to take effect
        // (configured for 1 s for this test).

        for (int i = 0; i < 150; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            group.clients[0]->process(timestamp, false);
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        group.add(_httpd);
        group.clients[2]->joinCall(cfg.build());
        ASSERT_TRUE(group.connectSingle(2, utils::Time::sec * 4));

        group.clients[2]->_audioSource->setFrequency(2100);
        group.clients[2]->_audioSource->setVolume(0.6);

        for (int i = 0; i < 100; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            group.clients[0]->process(timestamp, false);
            group.clients[2]->process(timestamp, false);
            _pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        group.clients[0]->stopRecording();
        group.clients[2]->stopRecording();

        group.clients[0]->stopTransports();
        group.clients[2]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = group.clients[0]->getAudioReceiveStats();
        const auto& rData2 = group.clients[1]->getAudioReceiveStats();
        const auto& rData3 = group.clients[2]->getAudioReceiveStats();

        EXPECT_EQ(rData1.size(), 2); // s2's audio, s3's audio
        EXPECT_EQ(rData2.size(), 1); // s1's audio, + padding
                                     // s1's audio, no padding, since it joined the call with no video.
        EXPECT_EQ(rData3.size(), 1);
    });
}

/*
Test setup:
1. Topology:
              Client-1 <----> SFU <------> Client-2
2. Control:
   SFU inbound networklink is set to lose packets (e.g. 1%) that comes from both clients.
3. Expectations:
   - both clients receive and serve NACK requests;
   - both clients have NO video packet loss, because they ARE retransmitted.
*/
TEST_F(IntegrationTest, packetLossVideoRecoveredViaNack)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        constexpr auto PACKET_LOSS_RATE = 0.04;

        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            linkInfo.second.ptrLink->setLossRate(PACKET_LOSS_RATE);
        }

        const std::string baseUrl = "http://127.0.0.1:8080";

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

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        group.clients[0]->stopTransports();
        group.clients[1]->stopTransports();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        RtxStats cumulativeStats;

        for (auto id : {0, 1})
        {
            // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
            // video streams only. So let's check SfuClient NACK-related stats instead:
            const auto stats = group.clients[id]->getCumulativeRtxStats();
            auto videoCounters = group.clients[id]->getCumulativeVideoReceiveCounters();
            cumulativeStats += stats;

            // Could happen that a key frame was sent after the packet that would be lost, in this case NACK would
            // have been ignored. So we might expect small number of videoCounters.lostPackets.
            if (videoCounters.lostPackets != 0)
            {
                // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
                // video streams only. So let's check SfuClient NACK-related stats instead:

                const auto stats = group.clients[id]->getCumulativeRtxStats();
                auto videoCounters = group.clients[id]->getCumulativeVideoReceiveCounters();

                // Could happen that a key frame was sent after the packet that would be lost, in this case NACK
                // would have been ignored. So we might expect small number of videoCounters.lostPackets.
                if (videoCounters.lostPackets != 0)
                {
                    logger::info(
                        "Client id: %s  packets missing: %zu, recovered: %zu, send: %zu, expected min recovery: %f",
                        "packetLossVideoRecoveredViaNack",
                        group.clients[id]->getLoggableId().c_str(),
                        stats.receiver.packetsMissing,
                        stats.receiver.packetsRecovered,
                        stats.sender.packetsSent,
                        stats.sender.packetsSent * PACKET_LOSS_RATE / 2);

                    ASSERT_TRUE(stats.receiver.packetsMissing >= stats.receiver.packetsRecovered);
                    // Expect number of non-recovered packet to be smaller than half the loss rate.
                    ASSERT_TRUE(stats.receiver.packetsMissing - stats.receiver.packetsRecovered <
                        stats.sender.packetsSent * PACKET_LOSS_RATE / 2);
                }
            }
        }

        // Expect, "as sender" we received several NACK request from SFU, and we served them all.
        EXPECT_NE(cumulativeStats.sender.nacksReceived, 0);
        EXPECT_NE(cumulativeStats.sender.retransmissionRequests, 0);
        EXPECT_NE(cumulativeStats.sender.retransmissions, 0);
        EXPECT_GE(cumulativeStats.sender.retransmissionRequests, cumulativeStats.sender.retransmissions);

        EXPECT_EQ(cumulativeStats.receiver.nackRequests, 0); // Expected as it's is not implemented yet.
        EXPECT_NE(cumulativeStats.receiver.packetsMissing, 0);
        EXPECT_NE(cumulativeStats.receiver.packetsRecovered, 0);
    });
}
