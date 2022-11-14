#include "test/integration/IntegrationTest.h"
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
#include "test/integration/FFTanalysis.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/HttpRequests.h"
#include "test/integration/emulator/Httpd.h"
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
#include <chrono>
#include <complex>
#include <memory>
#include <sstream>
#include <unordered_set>

IntegrationTest::IntegrationTest()
    : _httpd(nullptr),
      _sendAllocator(memory::packetPoolSize, "IntegrationTest"),
      _audioAllocator(memory::packetPoolSize, "IntegrationTestAudio"),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(4096, "testMain")),
      _sslDtls(nullptr),
      _network(transport::createRtcePoll()),
      _pacer(10 * utils::Time::ms),
      _instanceCounter(0),
      _numWorkerThreads(getNumWorkerThreads()),
      _clientsConnectionTimeout(15)
{
}

IntegrationTest::~IntegrationTest()
{
    delete _httpd;
}

// TimeTurner time source must be set before starting any threads.
// Fake internet thread, JobManager timer thread, worker threads.
void IntegrationTest::SetUp()
{
#ifdef NOPERF_TEST
    // GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;

    utils::Time::initialize(_timeSource);
    _httpd = new emulator::HttpdFactory();
    _internet = std::make_unique<fakenet::InternetRunner>(100 * utils::Time::us);
    _timers = std::make_unique<jobmanager::TimerQueue>(4096);
    _jobManager = std::make_unique<jobmanager::JobManager>(*_timers);
    for (size_t threadIndex = 0; threadIndex < getNumWorkerThreads(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager, true));
    }
}

void IntegrationTest::TearDown()
{
#ifdef NOPERF_TEST
    // GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    _bridge.reset();
    _transportFactory.reset();
    _timers->stop();
    _jobManager->stop();
    for (auto& worker : _workerThreads)
    {
        worker->stop();
    }

    if (_internet)
    {
        assert(!_internet->isRunning());
        _internet.reset();
    }

    logger::info("IntegrationTest torn down", "IntegrationTest");
}

size_t IntegrationTest::getNumWorkerThreads()
{
    const auto hardwareConcurrency = std::thread::hardware_concurrency();
    if (hardwareConcurrency == 0)
    {
        return 7;
    }
    return std::max(hardwareConcurrency - 1, 1U);
}

void IntegrationTest::initBridge(config::Config& config)
{
    _clientsEndpointFactory =
        std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_internet->getNetwork(),
            [](std::shared_ptr<fakenet::NetworkLink>, const transport::SocketAddress& addr, const std::string& name) {
                logger::info("Client %s endpoint uses address %s",
                    "IntegrationTest",
                    name.c_str(),
                    addr.toString().c_str());
            }));

    _bridge = std::make_unique<bridge::Bridge>(config);
    _bridgeEndpointFactory =
        std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_internet->getNetwork(),
            [this](std::shared_ptr<fakenet::NetworkLink> netLink,
                const transport::SocketAddress& addr,
                const std::string& name) {
                logger::info("Bridge: %s endpoint uses address %s",
                    "IntegrationTest",
                    name.c_str(),
                    addr.toString().c_str());
                this->_endpointNetworkLinkMap.emplace(name, NetworkLinkInfo{netLink.get(), addr});
            }));

    _bridge->initialize(_bridgeEndpointFactory, *_httpd);

    initLocalTransports();
}

void IntegrationTest::initLocalTransports()
{
    _sslDtls = &_bridge->getSslDtls();
    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);

    std::string configJson =
        "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10050, \"recording.singlePort\":0}";
    _config.readFromString(configJson);
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_config.ice.preferredIp, 0));

    _transportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _config,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControlConfig,
        interfaces,
        *_network,
        *_mainPoolAllocator,
        _clientsEndpointFactory);

    for (const auto& linkInfo : _endpointNetworkLinkMap)
    {
        // SFU's default downlinks is good (1 Gbps).
        linkInfo.second.ptrLink->setBandwidthKbps(1000000);
    }
}

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

std::vector<api::ConferenceEndpoint> IntegrationTest::getConferenceEndpointsInfo(emulator::HttpdFactory* httpd,
    const char* baseUrl)
{
    nlohmann::json responseBody;
    auto httpSuccess = emulator::awaitResponse<HttpGetRequest>(httpd,
        std::string(baseUrl) + "/conferences",
        500 * utils::Time::ms,
        responseBody);

    EXPECT_TRUE(httpSuccess);
    EXPECT_TRUE(responseBody.is_array());
    std::vector<std::string> confIds;
    responseBody.get_to(confIds);

    nlohmann::json endpointRequestBody;
    httpSuccess = emulator::awaitResponse<HttpGetRequest>(httpd,
        std::string(baseUrl) + "/conferences/" + confIds[0],
        5000 * utils::Time::ms,
        endpointRequestBody);

    EXPECT_TRUE(httpSuccess);
    EXPECT_TRUE(endpointRequestBody.is_array());

    return api::Parser::parseConferenceEndpoints(endpointRequestBody);
}

api::ConferenceEndpointExtendedInfo IntegrationTest::getEndpointExtendedInfo(emulator::HttpdFactory* httpd,
    const char* baseUrl,
    const std::string& endpointId)
{
    nlohmann::json responseBody;
    auto confRequest = emulator::awaitResponse<HttpGetRequest>(httpd,
        std::string(baseUrl) + "/conferences",
        500 * utils::Time::ms,
        responseBody);

    EXPECT_TRUE(confRequest);

    EXPECT_TRUE(responseBody.is_array());
    std::vector<std::string> confIds;
    responseBody.get_to(confIds);

    auto endpointRequest = emulator::awaitResponse<HttpGetRequest>(httpd,
        std::string(baseUrl) + "/conferences/" + confIds[0] + "/" + endpointId,
        500 * utils::Time::ms,
        responseBody);

    EXPECT_TRUE(endpointRequest);

    return api::Parser::parseEndpointExtendedInfo(responseBody);
}

bool IntegrationTest::isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint)
{
    auto it = std::find_if(endpoints.cbegin(), endpoints.cend(), [&endpoint](const api::ConferenceEndpoint& e) {
        return e.id == endpoint;
    });
    assert(it != endpoints.cend());
    return it->isActiveTalker;
}

template <typename TClient>
IntegrationTest::AudioAnalysisData IntegrationTest::analyzeRecording(TClient* client,
    double expectedDurationSeconds,
    size_t mixedAudioSources,
    bool dumpPcmData)
{
    constexpr auto AUDIO_PACKET_SAMPLE_COUNT = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    auto audioCounters = client->_transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
    EXPECT_EQ(audioCounters.lostPackets, 0);

    const auto& data = client->getAudioReceiveStats();
    IntegrationTest::AudioAnalysisData result;
    for (const auto& item : data)
    {
        if (client->isRemoteVideoSsrc(item.first))
        {
            continue;
        }

        result.audioSsrcCount++;

        std::vector<double> freqVector;
        std::vector<std::pair<uint64_t, double>> amplitudeProfile;
        auto rec = item.second->getRecording();
        ::analyzeRecording(rec,
            freqVector,
            amplitudeProfile,
            item.second->getLoggableId().c_str(),
            mixedAudioSources ? expectedDurationSeconds * utils::Time::ms : 0);

        if (mixedAudioSources)
        {
            EXPECT_EQ(freqVector.size(), mixedAudioSources);
            EXPECT_GE(rec.size(), expectedDurationSeconds * codec::Opus::sampleRate);
        }
        else
        {
            EXPECT_EQ(freqVector.size(), 1);
            EXPECT_NEAR(rec.size(), expectedDurationSeconds * codec::Opus::sampleRate, 3 * AUDIO_PACKET_SAMPLE_COUNT);

            EXPECT_EQ(amplitudeProfile.size(), 2);
            if (amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(amplitudeProfile[1].second, 5725, 125);
            }
        }

        result.dominantFrequencies.insert(result.dominantFrequencies.begin(), freqVector.begin(), freqVector.end());
        result.amplitudeProfile.insert(result.amplitudeProfile.begin(),
            amplitudeProfile.begin(),
            amplitudeProfile.end());
        if (dumpPcmData)
        {
            item.second->dumpPcmData();
        }
    }

    std::sort(result.dominantFrequencies.begin(), result.dominantFrequencies.end());
    return result;
}

void IntegrationTest::runTestInThread(const size_t expectedNumThreads, std::function<void()> test)
{
    // allow internet thread to forward packets next time it wakes up.
    if (_internet)
    {
        _internet->start();
    }

    // run test in thread that will also sleep at TimeTurner
    std::thread runner([test] { test(); });

    _timeSource.waitForThreadsToSleep(expectedNumThreads, 10 * utils::Time::sec);

    if (_internet)
    {
        // run for 80s or until test runner thread stops the time run
        _timeSource.runFor(80 * utils::Time::sec);

        // wait for all to sleep before switching time source
        _timeSource.waitForThreadsToSleep(expectedNumThreads, 10 * utils::Time::sec);

        // all threads are asleep. Switch to real time
        logger::info("Switching back to real time-space", "");
        utils::Time::initialize();

        // release all sleeping threads into real time to finish the test
        _timeSource.shutdown();
    }

    runner.join();
}

void IntegrationTest::startSimulation()
{
    _internet->start();
    utils::Time::nanoSleep(1 * utils::Time::sec);
}

void IntegrationTest::finalizeSimulationWithTimeout(uint64_t rampdownTimeout)
{
    // Stopped the internet, but allow some process to finish.
    const auto step = 5 * utils::Time::ms;
    const bool internetRunning = (_internet->getState() == fakenet::InternetRunner::State::running);

    for (uint64_t t = 0; t < rampdownTimeout && internetRunning; t += step)
    {
        utils::Time::nanoSleep(step);
    }

    _internet->pause();

    // stop time turner and it will await all threads to fall asleep, including me
    _timeSource.stop();
    utils::Time::nanoSleep(utils::Time::ms * 10);
}

void IntegrationTest::finalizeSimulation()
{
    finalizeSimulationWithTimeout(0);
}

TEST_F(IntegrationTest, plain)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, false);

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
            const auto data = analyzeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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

TEST_F(IntegrationTest, twoClientsAudioOnly)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, false, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, false, true);

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
            const auto data = analyzeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5);
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

TEST_F(IntegrationTest, audioOnlyNoPadding)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");

        printf("T%" PRIu64 " test running\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);
        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        printf("T%" PRIu64 " starting conf\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        group.startConference(conf, baseUrl + "/colibri");

        printf("T%" PRIu64 " calling\n", (utils::Time::rawAbsoluteTime() / utils::Time::ms) & 0xFFFF);
        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, false, false);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, false, false);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, false, false);

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

TEST_F(IntegrationTest, paddingOffWhenRtxNotProvided)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");

        initBridge(_config);
        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        using RtpVideoReceiver = typename SfuClient<ColibriChannel>::RtpVideoReceiver;

        AnswerOptions answerOptionNoRtx;
        answerOptionNoRtx.rtxDisabled = true;

        group.clients[0]->_channel.setAnswerOptions(answerOptionNoRtx);
        group.clients[1]->_channel.setAnswerOptions(answerOptionNoRtx);

        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);

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

TEST_F(IntegrationTest, videoOffPaddingOff)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        /*
           Test checks that after video is off and cooldown interval passed, no padding will be sent for the
           call that became audio-only.
        */

        _config.readFromString(
            "{\"ip\":\"127.0.0.1\", "
            "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\", \"rctl.cooldownInterval\":1}");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
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
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
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

TEST_F(IntegrationTest, plainNewApi)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

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
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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

TEST_F(IntegrationTest, ptime10)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

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
            std::string(baseUrl) + "/colibri/stats",
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
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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

TEST_F(IntegrationTest, detectIsPtt)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulationWithTimeout, this, utils::Time::sec));
        startSimulation();

        group.startConference(conf, baseUrl);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);

        auto connectResult = group.connectAll(utils::Time::sec * _clientsConnectionTimeout);
        ASSERT_TRUE(connectResult);
        if (!connectResult)
        {
            return;
        }

        group.clients[0]->_audioSource->setFrequency(600);
        group.clients[1]->_audioSource->setFrequency(1300);
        group.clients[2]->_audioSource->setFrequency(2100);

        group.clients[0]->_audioSource->setVolume(0.6);
        group.clients[1]->_audioSource->setVolume(0.6);
        group.clients[2]->_audioSource->setVolume(0.6);

        // Disable audio level extension, otherwise constant signal will lead to the 'noise leve' equal to the
        // signal and detection would fail
        group.clients[0]->_audioSource->setUseAudioLevel(false);
        group.clients[1]->_audioSource->setUseAudioLevel(false);
        group.clients[2]->_audioSource->setUseAudioLevel(false);

        // =============================== PART 1: #1 & #2 talking ====================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

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

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        group.run(utils::Time::sec * 2);
        utils::Time::nanoSleep(utils::Time::sec * 1);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 3: #3 talking =========================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);

        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 4: nobody talking =====================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        group.clients[2]->stopRecording();
        group.clients[1]->stopRecording();
        group.clients[0]->stopRecording();

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();
    });
};

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

        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        for (const auto& linkInfo : _endpointNetworkLinkMap)
        {
            linkInfo.second.ptrLink->setLossRate(PACKET_LOSS_RATE);
        }

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 2);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);

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

TEST_F(IntegrationTest, endpointAutoRemove)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);
        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true, 0);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true, 10);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true, 10);

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

        group.clients[2]->_transport->stop();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(2, endpoints.size());

        group.clients[1]->_transport->stop();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(_httpd, baseUrl.c_str());
        EXPECT_EQ(1, endpoints.size());

        group.clients[0]->_transport->stop();
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
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

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
        auto transport = _transportFactory->createOnPrivatePort(ice::IceRole::CONTROLLING, 256 * 1024, 1);

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
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 3);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl, false);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        const auto smbPort = group.clients[0]->_transport->getRemotePeer();
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

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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

TEST_F(IntegrationTest, neighbours)
{
    runTestInThread(expectedTestThreadCount(1), [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>>
            group(_httpd, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls, 4);

        Conference conf(_httpd);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        std::string neighbours[] = {"gid1"};
        utils::Span<std::string> span(neighbours);
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Opus, true, true);
        group.clients[1]->initiateCall2(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true, span);
        group.clients[2]->initiateCall2(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, true, span);

        group.clients[3]->initiateCall2(baseUrl, conf.getId(), false, emulator::Audio::Opus, true, false, span);

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
            results[id] = analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, chMixed[id]);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
            std::string clientName = "client_" + std::to_string(id);
            group.clients[id]->_transport->getReportSummary(transportSummary);
            logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);

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
