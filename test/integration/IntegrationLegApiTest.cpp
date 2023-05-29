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
#include "test/integration/emulator/ColibriChannel.h"
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
class IntegrationLegApi : public IntegrationTest
{
};

TEST_F(IntegrationLegApi, plain)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");
        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());
        group.clients[2]->joinCall(cfg.mixed().build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
            1500 * utils::Time::ms,
            responseBody);
        EXPECT_TRUE(statsSuccess);

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

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data =
                analyzeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5, true, 2 == id ? 2 : 0);
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
        }
    });
}

TEST_F(IntegrationLegApi, twoClientsAudioOnly)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).withOpus();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        nlohmann::json responseBody;
        auto statsSuccess = emulator::awaitResponse<HttpGetRequest>(_httpd,
            std::string(baseUrl) + "/colibri/stats",
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

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[2] = {1300.0, 600.0};
        size_t freqId = 0;
        for (auto id : {0, 1})
        {
            const auto data = analyzeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5, true);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_EQ(data.amplitudeProfile.size(), 2);
            if (data.amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(data.amplitudeProfile[1].second, 5725, 100);
            }
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId++], 25.0);
        }
    });
}

TEST_F(IntegrationLegApi, audioOnlyNoPadding)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        printf("T%" PRIu64 " test running\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            3);

        Conference conf(_httpd);
        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        printf("T%" PRIu64 " starting conf\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        group.startConference(conf, baseUrl + "/colibri");

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

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

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

TEST_F(IntegrationLegApi, paddingOffWhenRtxNotProvided)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(_defaultSmbConfig);

        initBridge(_config);
        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        using RtpVideoReceiver = typename SfuClient<ColibriChannel>::RtpVideoReceiver;

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

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

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

TEST_F(IntegrationLegApi, videoOffPaddingOff)
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

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

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
        group.clients[1]->_transport->stop();

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

        group.clients[0]->_transport->stop();
        group.clients[2]->_transport->stop();

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
TEST_F(IntegrationLegApi, packetLossVideoRecoveredViaNack)
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

        GroupCall<SfuClient<ColibriChannel>> group(_httpd,
            _instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_clientTransportFactory,
            *_sslDtls,
            2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        CallConfigBuilder cfg(conf.getId());
        cfg.url(baseUrl).av();

        group.clients[0]->initiateCall(cfg.build());
        group.clients[1]->joinCall(cfg.build());

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        RtxStats cumulativeStats;

        for (auto id : {0, 1})
        {
            // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
            // video streams only. So let's check SfuClient NACK-related stats instead:
            const auto stats = group.clients[id]->getCumulativeRtxStats();
            auto videoCounters = group.clients[id]->_transport->getCumulativeVideoReceiveCounters();
            cumulativeStats += stats;

            // Could happen that a key frame was sent after the packet that would be lost, in this case NACK would
            // have been ignored. So we might expect small number of videoCounters.lostPackets.
            if (videoCounters.lostPackets != 0)
            {
                // Can't rely on cumulative audio stats, since it might happen that all the losses were happening to
                // video streams only. So let's check SfuClient NACK-related stats instead:

                const auto stats = group.clients[id]->getCumulativeRtxStats();
                auto videoCounters = group.clients[id]->_transport->getCumulativeVideoReceiveCounters();

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
