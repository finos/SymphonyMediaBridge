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

#define USE_FAKENETWORK 1

IntegrationTest::IntegrationTest()
    : _sendAllocator(memory::packetPoolSize, "IntegrationTest"),
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

// TimeTurner time source must be set before starting any threads.
// Fake internet thread, JobManager timer thread, worker threads.
void IntegrationTest::SetUp()
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;

#if USE_FAKENETWORK
    utils::Time::initialize(_timeSource);
#endif

    _internet = std::make_unique<fakenet::InternetRunner>(100 * utils::Time::us);
    _jobManager = std::make_unique<jobmanager::JobManager>();
    for (size_t threadIndex = 0; threadIndex < getNumWorkerThreads(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }

#if USE_FAKENETWORK
    _clientsEndpointFactory =
        std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_internet->get(),
            [](std::shared_ptr<fakenet::NetworkLink>, const transport::SocketAddress& addr, const std::string& name) {
                logger::info("Client %s endpoint uses address %s",
                    "IntegrationTest",
                    name.c_str(),
                    addr.toString().c_str());
            }));
#else
    _clientsEndpointFactory = std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl());
#endif
}

void IntegrationTest::TearDown()
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    _bridge.reset();
    _transportFactory.reset();
    _jobManager->stop();
    for (auto& worker : _workerThreads)
    {
        worker->stop();
    }

    assert(!_internet->isRunning());
    _internet.reset();

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
    _bridge = std::make_unique<bridge::Bridge>(config);
#if USE_FAKENETWORK
    _bridgeEndpointFactory =
        std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_internet->get(),
            [this](std::shared_ptr<fakenet::NetworkLink> netLink,
                const transport::SocketAddress& addr,
                const std::string& name) {
                logger::info("Bridge: %s endpoint uses address %s",
                    "IntegrationTest",
                    name.c_str(),
                    addr.toString().c_str());
                this->_endpointNetworkLinkMap.emplace(name, NetworkLinkInfo{netLink.get(), addr});
            }));
#else
    _endpointFacory = std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl());
#endif
    _bridge->initialize(_bridgeEndpointFactory);

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

std::vector<api::ConferenceEndpoint> IntegrationTest::getConferenceEndpointsInfo(const char* baseUrl)
{
    HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    EXPECT_TRUE(confRequest.getJsonBody().is_array());
    std::vector<std::string> confIds;
    confRequest.getJsonBody().get_to(confIds);

    HttpGetRequest endpointRequest((std::string(baseUrl) + "/conferences/" + confIds[0]).c_str());
    endpointRequest.awaitResponse(50000 * utils::Time::ms);
    EXPECT_TRUE(endpointRequest.isSuccess());
    EXPECT_TRUE(endpointRequest.getJsonBody().is_array());

    return api::Parser::parseConferenceEndpoints(endpointRequest.getJsonBody());
}

api::ConferenceEndpointExtendedInfo IntegrationTest::getEndpointExtendedInfo(const char* baseUrl,
    const std::string& endpointId)
{
    HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    EXPECT_TRUE(confRequest.getJsonBody().is_array());
    std::vector<std::string> confIds;
    confRequest.getJsonBody().get_to(confIds);

    HttpGetRequest endpointRequest((std::string(baseUrl) + "/conferences/" + confIds[0] + "/" + endpointId).c_str());
    endpointRequest.awaitResponse(50000 * utils::Time::ms);
    EXPECT_TRUE(endpointRequest.isSuccess());

    return api::Parser::parseEndpointExtendedInfo(endpointRequest.getJsonBody());
}

void IntegrationTest::analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint64_t cutAtTime)
{
    utils::RateTracker<5> amplitudeTracker(codec::Opus::sampleRate / 10);
    const size_t fftWindowSize = 2048;
    std::valarray<std::complex<double>> testVector(fftWindowSize);

    const auto limit = cutAtTime == 0 ? recording.size() : cutAtTime * codec::Opus::sampleRate / utils::Time::ms;
    const auto size = recording.size() > limit ? limit : recording.size();

    for (size_t t = 0; t < size; ++t)
    {
        amplitudeTracker.update(std::abs(recording[t]), t);
        if (t > codec::Opus::sampleRate / 10)
        {
            if (amplitudeProfile.empty() ||
                (t - amplitudeProfile.back().first > codec::Opus::sampleRate / 10 &&
                    std::abs(amplitudeProfile.back().second - amplitudeTracker.get(t, codec::Opus::sampleRate / 5)) >
                        100))
            {
                amplitudeProfile.push_back(std::make_pair(t, amplitudeTracker.get(t, codec::Opus::sampleRate / 5)));
            }
        }
    }

    if (size < fftWindowSize)
    {
        return;
    }

    for (size_t cursor = 0; cursor < size - fftWindowSize; cursor += 256)
    {
        for (uint64_t x = 0; x < fftWindowSize; ++x)
        {
            testVector[x] = std::complex<double>(static_cast<double>(recording[x + cursor]), 0.0) / (256.0 * 128);
        }

        SampleDataUtils::fft(testVector);

        std::vector<double> frequencies;
        SampleDataUtils::listFrequencies(testVector, codec::Opus::sampleRate, frequencies);
        for (size_t i = 0; i < frequencies.size() && i < 50; ++i)
        {
            if (std::find(frequencyPeaks.begin(), frequencyPeaks.end(), frequencies[i]) == frequencyPeaks.end())
            {
                logger::debug("added new freq %.3f", logId, frequencies[i]);
                frequencyPeaks.push_back(frequencies[i]);
            }
        }
    }
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
IntegrationTest::AudioAnalysisData IntegrationTest::analizeRecording(TClient* client,
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
        analyzeRecording(rec,
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
                EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
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
#if USE_FAKENETWORK
    _internet->start();
#endif

    // run test in thread that will also sleep at TimeTurner
    std::thread runner([test] { test(); });

    _timeSource.waitForThreadsToSleep(expectedNumThreads, 10 * utils::Time::sec);

#if USE_FAKENETWORK

    // run for 80s or until test runner thread stops the time run
    _timeSource.runFor(80 * utils::Time::sec);

    // all threads are asleep. Switch to real time
    logger::info("Switching back to real time-space", "");
    utils::Time::initialize();

    // release all sleeping threads into real time to finish the test
    _timeSource.shutdown();
#endif
    runner.join();
    logger::debug("test out", "");
}

void IntegrationTest::startSimulation()
{
#if USE_FAKENETWORK
    _internet->start();
    utils::Time::nanoSleep(1 * utils::Time::sec);
#endif
}

void IntegrationTest::finalizeSimulationWithTimeout(uint64_t rampdownTimeout)
{
    _internet->pause();

    // Stopped the internet, but allow some process to finish.
    const auto now = _timeSource.getAbsoluteTime();
    const auto step = 500 * utils::Time::us;
    for (auto t = now; t < now + rampdownTimeout; t += step)
    {
        utils::Time::nanoSleep(step);
    }

    while (!_internet->isPaused())
    {
        utils::Time::nanoSleep(utils::Time::ms * 10);
    }

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
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/colibri/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());

        auto endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
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
            const auto data = analizeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            2);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, false, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, false, true);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/colibri/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());

        auto endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
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
            const auto data = analizeRecording<SfuClient<ColibriChannel>>(group.clients[id].get(), 5);
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
    runTestInThread(2 * _numWorkerThreads + 7, [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, false, false);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, false, false);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, false, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

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
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");

        initBridge(_config);
        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        using RtpVideoReceiver = typename SfuClient<ColibriChannel>::RtpVideoReceiver;

        AnswerOptions answerOptionNoRtx;
        answerOptionNoRtx.rtxDisabled = true;

        group.clients[0]->_channel.setAnswerOptions(answerOptionNoRtx);
        group.clients[1]->_channel.setAnswerOptions(answerOptionNoRtx);

        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, true);

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
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        /*
           Test checks that after video is off and cooldown interval passed, no padding will be sent for the
           call that became audio-only.
        */

        _config.readFromString(
            "{\"ip\":\"127.0.0.1\", "
            "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\", \"rctl.cooldownInterval\":1}");

        initBridge(_config);

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            2);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        // Audio only for all three participants.
        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
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

        group.add();
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
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
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const double expectedFrequencies[3][2] = {{1300.0, 2100.0}, {600.0, 2100.0}, {600.0, 1300.0}};
        size_t freqId = 0;
        for (auto id : {0, 1, 2})
        {
            const auto data = analizeRecording<SfuClient<Channel>>(group.clients[id].get(), 5, 2 == id ? 2 : 0);
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

TEST_F(IntegrationTest, ptime10)
{
    runTestInThread(2 * _numWorkerThreads + 6, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);
        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, false);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        group.clients[0]->_transport->stop();
        group.clients[1]->_transport->stop();
        group.clients[2]->_transport->stop();

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = group.clients[0]->_transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = group.clients[0]->getAudioReceiveStats();
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
                analyzeRecording(rec,
                    freqVector,
                    amplitudeProfile,
                    item.second->getLoggableId().c_str(),
                    5 * utils::Time::ms);
                EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 3 * audioPacketSampleCount);
                EXPECT_EQ(freqVector.size(), 1);
                allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

                EXPECT_EQ(amplitudeProfile.size(), 2);
                if (amplitudeProfile.size() > 1)
                {
                    EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
                }

                // item.second->dumpPcmData();
            }

            std::sort(allFreq.begin(), allFreq.end());
            EXPECT_NEAR(allFreq[0], 1300.0, 25.0);
            EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
        }
        {
            auto audioCounters = group.clients[1]->_transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData2 = group.clients[1]->getAudioReceiveStats();
            EXPECT_EQ(rData2.size(), 2);
            std::vector<double> allFreq;
            for (const auto& item : rData2)
            {
                if (group.clients[1]->isRemoteVideoSsrc(item.first))
                {
                    continue;
                }

                std::vector<double> freqVector;
                std::vector<std::pair<uint64_t, double>> amplitudeProfile;
                auto rec = item.second->getRecording();
                analyzeRecording(rec,
                    freqVector,
                    amplitudeProfile,
                    item.second->getLoggableId().c_str(),
                    5 * utils::Time::ms);
                EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 3 * audioPacketSampleCount);
                EXPECT_EQ(freqVector.size(), 1);
                allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

                EXPECT_EQ(amplitudeProfile.size(), 2);
                if (amplitudeProfile.size() > 1)
                {
                    EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
                }

                // item.second->dumpPcmData();
            }

            std::sort(allFreq.begin(), allFreq.end());
            EXPECT_NEAR(allFreq[0], 600.0, 25.0);
            EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
        }
        {
            auto audioCounters = group.clients[2]->_transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData3 = group.clients[2]->getAudioReceiveStats();
            // We expect one audio ssrc
            EXPECT_EQ(rData3.size(), 1);
            size_t audioSsrcCount = 0;
            for (const auto& item : rData3)
            {
                if (group.clients[2]->isRemoteVideoSsrc(item.first))
                {
                    continue;
                }

                ++audioSsrcCount;

                std::vector<double> freqVector;
                std::vector<std::pair<uint64_t, double>> amplitudeProfile;
                auto rec = item.second->getRecording();
                analyzeRecording(rec,
                    freqVector,
                    amplitudeProfile,
                    item.second->getLoggableId().c_str(),
                    5 * utils::Time::ms);

                std::sort(freqVector.begin(), freqVector.end());
                EXPECT_EQ(freqVector.size(), 2);
                if (freqVector.size() == 2)
                {
                    EXPECT_NEAR(freqVector[0], 600.0, 25.0);
                    EXPECT_NEAR(freqVector[1], 1300.0, 25.0);
                }

                EXPECT_GE(amplitudeProfile.size(), 2);
                for (auto& item : amplitudeProfile)
                {
                    logger::debug("%.3fs, %.3f", "", item.first / 48000.0, item.second);
                }
                // We expect a ramp-up of volume like this:
                // start from 0;
                // ramp-up to about 1826 (+-250) in 0.8s (+-0,2s)
                if (amplitudeProfile.size() >= 2)
                {
                    EXPECT_EQ(amplitudeProfile[0].second, 0);

                    EXPECT_NEAR(amplitudeProfile.back().second, 1826, 250);
                    EXPECT_NEAR(amplitudeProfile.back().first, 48000 * 0.8, 48000 * 0.2);
                }

                // item.second->dumpPcmData();
            }

            EXPECT_EQ(audioSsrcCount, 1);
        }
    });
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

TEST_F(IntegrationTest, simpleBarbell)
{
    runTestInThread(3 * _numWorkerThreads + 10, [this]() {
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
        Conference conf2;

        Barbell bb1;
        Barbell bb2;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl2, conf2.getId(), false, true, true, true);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

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

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", *group.clients[0]);
        logVideoSent("client2", *group.clients[1]);
        logVideoSent("client3", *group.clients[2]);

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = group.clients[0]->_transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = group.clients[0]->getAudioReceiveStats();
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
                EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 3 * audioPacketSampleCount);
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

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary3;
            auto videoReceiveStats = group.clients[0]->_transport->getCumulativeVideoReceiveCounters();
            group.clients[1]->_transport->getReportSummary(transportSummary2);
            group.clients[2]->_transport->getReportSummary(transportSummary3);

            logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
            logTransportSummary("client2", group.clients[1]->_transport.get(), transportSummary2);
            logTransportSummary("client3", group.clients[2]->_transport.get(), transportSummary3);

            EXPECT_NEAR(videoReceiveStats.packets,
                transportSummary2.begin()->second.packetsSent + transportSummary3.begin()->second.packetsSent,
                25);
        }
        {
            auto audioCounters = group.clients[1]->_transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData1 = group.clients[1]->getAudioReceiveStats();
            std::vector<double> allFreq;
            for (const auto& item : rData1)
            {
                std::vector<double> freqVector;
                std::vector<std::pair<uint64_t, double>> amplitudeProfile;
                auto rec = item.second->getRecording();
                analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());
                EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 3 * audioPacketSampleCount);
                EXPECT_EQ(freqVector.size(), 1);
                allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

                EXPECT_EQ(amplitudeProfile.size(), 2);
                if (amplitudeProfile.size() > 1)
                {
                    EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
                }

                // item.second->dumpPcmData();
            }

            std::sort(allFreq.begin(), allFreq.end());
            EXPECT_NEAR(allFreq[0], 600.0, 25.0);
            EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
        }
    });
}

TEST_F(IntegrationTest, barbellAfterClients)
{
    runTestInThread(3 * _numWorkerThreads + 11, [this]() {
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

        utils::Time::nanoSleep(1 * utils::Time::sec);

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            2);

        Conference conf;
        Conference conf2;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl);
        group.startConference(conf2, baseUrl2);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl2, conf2.getId(), false, true, true, true);

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

        Barbell bb1;
        Barbell bb2;

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        group.run(utils::Time::ms * 5000);

        group.clients[1]->stopRecording();
        group.clients[0]->stopRecording();

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

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
            const auto data = analizeRecording<SfuClient<Channel>>(group.clients[id].get(), 5);
            EXPECT_EQ(data.dominantFrequencies.size(), 1);
            EXPECT_EQ(data.amplitudeProfile.size(), 2);
            if (data.amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(data.amplitudeProfile[1].second, 5725, 100);
            }
            ASSERT_GE(data.dominantFrequencies.size(), 1);
            EXPECT_NEAR(data.dominantFrequencies[0], expectedFrequencies[freqId++], 25.0);
        }

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
        auto videoReceiveStats = group.clients[0]->_transport->getCumulativeVideoReceiveCounters();
        group.clients[1]->_transport->getReportSummary(transportSummary);

        logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
        logTransportSummary("client2", group.clients[1]->_transport.get(), transportSummary);

        EXPECT_NEAR(videoReceiveStats.packets, transportSummary.begin()->second.packetsSent, 25);
    });
}

TEST_F(IntegrationTest, detectIsPtt)
{
    runTestInThread(2 * _numWorkerThreads + 7, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        const auto baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulationWithTimeout, this, utils::Time::sec));
        startSimulation();

        group.startConference(conf, baseUrl);

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, true);

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

        // Disable audio level extension, otherwise constant signal will lead to the 'noise leve' equal to the signal
        // and detection would fail
        group.clients[0]->_audioSource->setUseAudioLevel(false);
        group.clients[1]->_audioSource->setUseAudioLevel(false);
        group.clients[2]->_audioSource->setUseAudioLevel(false);

        // =============================== PART 1: #1 & #2 talking ====================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        group.run(utils::Time::sec * 2);

        auto endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        auto endpointExtendedInfo = getEndpointExtendedInfo(baseUrl, endpoints[0].id);

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

        endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 3: #3 talking =========================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);

        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[0]->_channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, group.clients[1]->_channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, group.clients[2]->_channel.getEndpointId()));

        // =============================== PART 4: nobody talking =====================

        group.clients[0]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[1]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        group.clients[2]->_audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        group.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(baseUrl);
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
    runTestInThread(2 * _numWorkerThreads + 7, [this]() {
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

        GroupCall<SfuClient<ColibriChannel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            2);

        Conference conf;

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true);

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

                // Could happen that a key frame was sent after the packet that would be lost, in this case NACK would
                // have been ignored. So we might expect small number of videoCounters.lostPackets.
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
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        GroupCall<SfuClient<Channel>> group(_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            3);

        Conference conf;
        group.startConference(conf, baseUrl + "/colibri");

        group.clients[0]->initiateCall(baseUrl, conf.getId(), true, true, true, true, 0);
        group.clients[1]->initiateCall(baseUrl, conf.getId(), false, true, true, true, 10);
        group.clients[2]->initiateCall(baseUrl, conf.getId(), false, true, true, true, 10);

        ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

        make5secCallWithDefaultAudioProfile(group);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/colibri/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        auto endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
        EXPECT_EQ(3, endpoints.size());

        group.clients[2]->_transport->stop();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
        EXPECT_EQ(2, endpoints.size());

        group.clients[1]->_transport->stop();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
        EXPECT_EQ(1, endpoints.size());

        group.clients[0]->_transport->stop();
        group.run(utils::Time::sec * 11);
        endpoints = getConferenceEndpointsInfo(baseUrl.c_str());
        EXPECT_EQ(1, endpoints.size());

        group.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();
    });
}

TEST_F(IntegrationTest, probing)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }

    runTestInThread(2 * _numWorkerThreads + 7, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        // Retrieve probing ICE candidates
        const std::string baseUrl = "http://127.0.0.1:8080";
        HttpGetRequest candidatesRequest((baseUrl + "/ice-candidates").c_str());
        candidatesRequest.awaitResponse(5 * utils::Time::sec);
        EXPECT_TRUE(candidatesRequest.isSuccess());

        ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
        startSimulation();

        const auto& data = candidatesRequest.getJsonBody();
        const auto& iceJson = data["ice"];

        logger::debug("%s", "", iceJson.dump(4).c_str());

        api::EndpointDescription::Ice ice = api::Parser::parseIce(iceJson);
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
