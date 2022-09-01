#include "test/integration/IntegrationTest.h"
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

#define DEFINE_3_CLIENT_CONFERENCE(TChannel, BASE_URL)                                                                 \
    Conference conf;                                                                                                   \
    conf.create(BASE_URL);                                                                                             \
    EXPECT_TRUE(conf.isSuccess());                                                                                     \
    utils::Time::rawNanoSleep(1 * utils::Time::sec);                                                                   \
    SfuClient<TChannel> client1(++_instanceCounter,                                                                    \
        *_mainPoolAllocator,                                                                                           \
        _audioAllocator,                                                                                               \
        *_transportFactory,                                                                                            \
        *_sslDtls);                                                                                                    \
    SfuClient<TChannel> client2(++_instanceCounter,                                                                    \
        *_mainPoolAllocator,                                                                                           \
        _audioAllocator,                                                                                               \
        *_transportFactory,                                                                                            \
        *_sslDtls);                                                                                                    \
    SfuClient<TChannel> client3(++_instanceCounter,                                                                    \
        *_mainPoolAllocator,                                                                                           \
        _audioAllocator,                                                                                               \
        *_transportFactory,                                                                                            \
        *_sslDtls);

IntegrationTest::IntegrationTest()
    : _sendAllocator(memory::packetPoolSize, "IntegrationTest"),
      _audioAllocator(memory::packetPoolSize, "IntegrationTestAudio"),
      _jobManager(std::make_unique<jobmanager::JobManager>()),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(4096, "testMain")),
      _sslDtls(nullptr),
      _network(transport::createRtcePoll()),
      _pacer(10 * utils::Time::ms),
      _instanceCounter(0),
      _numWorkerThreads(getNumWorkerThreads())
{
}

void IntegrationTest::SetUp()
{

    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        GTEST_SKIP();
    }
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;

    _internet = std::make_unique<fakenet::InternetRunner>(100 * utils::Time::us);

#if USE_FAKENETWORK
    utils::Time::initialize(_timeSource);
#endif

    for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }
}

void IntegrationTest::TearDown()
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        GTEST_SKIP();
    }
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
    _endpointFacory = std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_internet->get()));
#else
    _endpointFacory = std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl());
#endif
    _bridge->initialize(_endpointFacory);

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
        _endpointFacory);
}

namespace
{
void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint64_t cutAtTime = 0)
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
} // namespace
using namespace emulator;

template <typename TChannel>
void make5secCallWithDefaultAudioProfile(SfuClient<TChannel>& client1,
    SfuClient<TChannel>& client2,
    SfuClient<TChannel>& client3,
    GroupCall<SfuClient<TChannel>>& groupCall)
{
    auto connectResult = groupCall.connect(utils::Time::sec * 5);
    ASSERT_TRUE(connectResult);
    if (!connectResult)
    {
        return;
    }

    client1._audioSource->setFrequency(600);
    client2._audioSource->setFrequency(1300);
    client3._audioSource->setFrequency(2100);

    client1._audioSource->setVolume(0.6);
    client2._audioSource->setVolume(0.6);
    client3._audioSource->setVolume(0.6);

    groupCall.run(utils::Time::sec * 5);
    utils::Time::nanoSleep(utils::Time::sec * 1);

    client3.stopRecording();
    client2.stopRecording();
    client1.stopRecording();
}

std::vector<api::ConferenceEndpoint> getConferenceEndpointsInfo(const char* baseUrl)
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

api::ConferenceEndpointExtendedInfo getEndpointExtendedInfo(const char* baseUrl, const std::string& endpointId)
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

bool isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint)
{
    auto it = std::find_if(endpoints.cbegin(), endpoints.cend(), [&endpoint](const api::ConferenceEndpoint& e) {
        return e.id == endpoint;
    });
    assert(it != endpoints.cend());
    return it->isActiveTalker;
}

void IntegrationTest::runTestInThread(const size_t expectedNumThreads, std::function<void()> test)
{
#if USE_FAKENETWORK
    // Start internet here, before runFor(...) so it goes into "nanoSleep".
    _internet->start();
#endif

    std::thread runner([test] { test(); });

    _timeSource.waitForThreadsToSleep(expectedNumThreads, 5 * utils::Time::sec);

#if USE_FAKENETWORK
    _timeSource.runFor(80 * utils::Time::sec);
#endif

    runner.join();
}
void IntegrationTest::startSimulation()
{
#if USE_FAKENETWORK
    _internet->start();
    utils::Time::nanoSleep(1 * utils::Time::sec);
#endif
}

void IntegrationTest::finalizeSimulation()
{
    _internet->stop();
    utils::Time::initialize();
    _timeSource.shutdown();
}

TEST_F(IntegrationTest, plain)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

        GroupCall<SfuClient<ColibriChannel>> groupCall({&client1, &client2, &client3});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

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

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = client1.getAudioReceiveStats();
            std::vector<double> allFreq;

            for (const auto& item : rData1)
            {
                if (client1.isRemoteVideoSsrc(item.first))
                {
                    continue;
                }

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
            EXPECT_NEAR(allFreq[0], 1300.0, 25.0);
            EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
        }
        {
            auto audioCounters = client2._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData2 = client2.getAudioReceiveStats();
            std::vector<double> allFreq;
            for (const auto& item : rData2)
            {
                if (client2.isRemoteVideoSsrc(item.first))
                {
                    continue;
                }

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
        {
            auto audioCounters = client3._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData3 = client3.getAudioReceiveStats();
            // We expect one audio ssrc
            EXPECT_EQ(rData3.size(), 1);
            size_t audioSsrcCount = 0;
            for (const auto& item : rData3)
            {
                if (client3.isRemoteVideoSsrc(item.first))
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
                EXPECT_NEAR(freqVector[0], 600.0, 25.0);
                EXPECT_NEAR(freqVector[1], 1300.0, 25.0);

                EXPECT_GE(amplitudeProfile.size(), 2);
                for (auto& item : amplitudeProfile)
                {
                    logger::debug("%.3fs, %.3f", "", item.first / 48000.0, item.second);
                }
                // We expect a ramp-up of volume like this:
                // start from 0;
                // ramp-up to about 1826 (+-250) in 0.8 (+-0,2s)
                if (amplitudeProfile.size() >= 2)
                {
                    EXPECT_EQ(amplitudeProfile[0].second, 0);

                    EXPECT_NEAR(amplitudeProfile.back().second, 1826, 250);
                    EXPECT_NEAR(amplitudeProfile.back().first, 48000 * 0.79, 48000 * 0.2);
                }

                // item.second->dumpPcmData();
            }

            EXPECT_EQ(audioSsrcCount, 1);
        }
    });
}

TEST_F(IntegrationTest, audioOnlyNoPadding)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
        initBridge(_config);

        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

        GroupCall<SfuClient<ColibriChannel>> groupCall({&client1, &client2, &client3});

        // Audio only for all three participants.
        client1.initiateCall(baseUrl, conf.getId(), true, true, false, false);
        client2.initiateCall(baseUrl, conf.getId(), false, true, false, false);
        client3.initiateCall(baseUrl, conf.getId(), false, true, false, false);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

        client3.stopRecording();
        client2.stopRecording();
        client1.stopRecording();

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = client1.getAudioReceiveStats();
        const auto& rData2 = client2.getAudioReceiveStats();
        const auto& rData3 = client3.getAudioReceiveStats();
        // We expect only one ssrc (audio), since padding (that comes on video ssrc) is disabled for audio only
        // calls).
        EXPECT_EQ(rData1.size(), 1);
        EXPECT_EQ(rData2.size(), 1);
        EXPECT_EQ(rData3.size(), 1);
    });
}

TEST_F(IntegrationTest, paddingOffWhenRtxNotProvided)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString("{\"ip\":\"127.0.0.1\", "
                               "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");

        initBridge(_config);

        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

        GroupCall<SfuClient<ColibriChannel>> groupCall({&client1, &client2, &client3});

        using RtpVideoReceiver = typename SfuClient<ColibriChannel>::RtpVideoReceiver;

        AnswerOptions answerOptionNoRtx;
        answerOptionNoRtx.rtxDisabled = true;

        client1._channel.setAnswerOptions(answerOptionNoRtx);
        client2._channel.setAnswerOptions(answerOptionNoRtx);

        // Audio only for all three participants.
        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, true);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = client1.getAudioReceiveStats();
        const auto& rData2 = client2.getAudioReceiveStats();
        const auto& rData3 = client3.getAudioReceiveStats();

        EXPECT_EQ(rData1.size(), 2); // s2's audio, s3's audio
        EXPECT_EQ(rData2.size(), 2); // s1's audio, s3's audio
        EXPECT_EQ(rData3.size(), 2); // s1's audio, s2's audio

        auto videoReceivers1 = client1.collectReceiversWithPackets();
        auto videoReceivers2 = client2.collectReceiversWithPackets();
        auto videoReceivers3 = client3.collectReceiversWithPackets();

        ASSERT_EQ(videoReceivers1.size(), 2); // s2's video, s3's video
        ASSERT_EQ(videoReceivers2.size(), 2); // s1's video, s3's video
        ASSERT_EQ(videoReceivers3.size(), 3); // s1's video, s3's video + padding

        // Check if all others have received video content only
        for (auto* videoReceiver : videoReceivers1)
        {
            ASSERT_GT(videoReceiver->videoPacketCount, 0); // It should contain video
            ASSERT_EQ(videoReceiver->rtxPacketCount, 0); // It should NOT contain rtx
            ASSERT_EQ(videoReceiver->unknownPayloadPacketCount, 0); // It should NOT video have unknown payload types
            ASSERT_EQ(videoReceiver->getContent(), RtpVideoReceiver::VideoContent::VIDEO);
        }

        for (auto* videoReceiver : videoReceivers2)
        {
            ASSERT_GT(videoReceiver->videoPacketCount, 0); // It should contain video
            ASSERT_EQ(videoReceiver->rtxPacketCount, 0); // It should NOT contain rtx
            ASSERT_EQ(videoReceiver->unknownPayloadPacketCount, 0); // It should NOT video have unknown payload types
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
        ASSERT_EQ(localVideoReceiver->unknownPayloadPacketCount, 0); // It should NOT video have unknown payload types
    });
}

TEST_F(IntegrationTest, videoOffPaddingOff)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        /*
           Test checks that after video is off and cooldown interval passed, no padding will be sent for the
           call that became audio-only.
        */

        _config.readFromString(
            "{\"ip\":\"127.0.0.1\", "
            "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\", \"rctl.cooldownInterval\":1}");

        initBridge(_config);

        startSimulation();

        const std::string baseUrl = "http://127.0.0.1:8080";

        DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

        GroupCall<SfuClient<ColibriChannel>> groupCall({&client1, &client2});

        // Audio only for all three participants.
        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        // Client 3 will join after client 2, which produce video, will leave.
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, true);

        if (!groupCall.connect(utils::Time::sec * 8))
        {
            EXPECT_TRUE(false);
            return;
        }

        // Have to produce some audio volume above "silence threshold", otherwise audio packets
        // won't be forwarded by SFU.
        client1._audioSource->setFrequency(600);
        client2._audioSource->setFrequency(1300);

        client1._audioSource->setVolume(0.6);
        client2._audioSource->setVolume(0.6);

        for (int i = 0; i < 100; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            client1.process(timestamp, false);
            client2.process(timestamp, true);
            _pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        client2.stopRecording();
        client2._transport->stop();

        // Video producer (client2) stopped, waiting 1.5s for rctl.cooldownInterval timeout to take effect
        // (configured for 1 s for this test).

        for (int i = 0; i < 150; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            client1.process(timestamp, false);
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        // client 3 joins.
        client3.processOffer();
        client3.connect();

        while (!client3._transport->isConnected())
        {
            utils::Time::nanoSleep(1 * utils::Time::sec);
            logger::debug("waiting for connect...", "test");
        }

        client3._audioSource->setFrequency(2100);
        client3._audioSource->setVolume(0.6);

        for (int i = 0; i < 100; ++i)
        {
            const auto timestamp = utils::Time::getAbsoluteTime();
            client1.process(timestamp, false);
            client3.process(timestamp, false);
            _pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
        }

        client1.stopRecording();
        client3.stopRecording();

        client1._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto& rData1 = client1.getAudioReceiveStats();
        const auto& rData2 = client2.getAudioReceiveStats();
        const auto& rData3 = client3.getAudioReceiveStats();

        EXPECT_EQ(rData1.size(), 2); // s2's audio, s3's audio
        EXPECT_EQ(rData2.size(), 1); // s1's audio, + padding
                                     // s1's audio, no padding, since it joined the call with no video.
        EXPECT_EQ(rData3.size(), 1);
    });
}

TEST_F(IntegrationTest, plainNewApi)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        startSimulation();

        const auto baseUrl = "http://127.0.0.1:8080";

        Conference conf;
        conf.create(baseUrl);
        EXPECT_TRUE(conf.isSuccess());
        utils::Time::nanoSleep(1 * utils::Time::sec);

        SfuClient<Channel> client1(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client2(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client3(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);

        GroupCall<SfuClient<Channel>> groupCall({&client1, &client2, &client3});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = client1.getAudioReceiveStats();
            std::vector<double> allFreq;

            for (const auto& item : rData1)
            {
                if (client1.isRemoteVideoSsrc(item.first))
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
            auto audioCounters = client2._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData2 = client2.getAudioReceiveStats();
            std::vector<double> allFreq;
            for (const auto& item : rData2)
            {
                if (client2.isRemoteVideoSsrc(item.first))
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
            auto audioCounters = client3._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData3 = client3.getAudioReceiveStats();
            // We expect one audio ssrc
            EXPECT_EQ(rData3.size(), 1);
            size_t audioSsrcCount = 0;
            for (const auto& item : rData3)
            {
                if (client3.isRemoteVideoSsrc(item.first))
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

TEST_F(IntegrationTest, ptime10)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        startSimulation();

        const auto baseUrl = "http://127.0.0.1:8080";

        Conference conf;
        conf.create(baseUrl);
        EXPECT_TRUE(conf.isSuccess());
        utils::Time::nanoSleep(1 * utils::Time::sec);

        SfuClient<Channel> client1(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls,
            10);
        SfuClient<Channel> client2(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client3(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);

        GroupCall<SfuClient<Channel>> groupCall({&client1, &client2, &client3});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = client1.getAudioReceiveStats();
            std::vector<double> allFreq;
            EXPECT_EQ(rData1.size(), 2);

            for (const auto& item : rData1)
            {
                if (client1.isRemoteVideoSsrc(item.first))
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
            auto audioCounters = client2._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData2 = client2.getAudioReceiveStats();
            EXPECT_EQ(rData2.size(), 2);
            std::vector<double> allFreq;
            for (const auto& item : rData2)
            {
                if (client2.isRemoteVideoSsrc(item.first))
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
            auto audioCounters = client3._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData3 = client3.getAudioReceiveStats();
            // We expect one audio ssrc
            EXPECT_EQ(rData3.size(), 1);
            size_t audioSsrcCount = 0;
            for (const auto& item : rData3)
            {
                if (client3.isRemoteVideoSsrc(item.first))
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
    runTestInThread(_numWorkerThreads + 4, [this]() {
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
        bridge2->initialize(_endpointFacory);

        startSimulation();

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        Conference conf;
        conf.create(baseUrl);
        EXPECT_TRUE(conf.isSuccess());

        Conference conf2;
        conf2.create(baseUrl2);
        EXPECT_TRUE(conf2.isSuccess());

        utils::Time::nanoSleep(1 * utils::Time::sec);

        Barbell bb1;
        Barbell bb2;

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        SfuClient<Channel> client1(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client2(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client3(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);

        GroupCall<SfuClient<Channel>> groupCall({&client1, &client2, &client3});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl2, conf2.getId(), false, true, true, true);
        client3.initiateCall(baseUrl2, conf2.getId(), false, true, true, true);

        make5secCallWithDefaultAudioProfile(client1, client2, client3, groupCall);

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        bb1.remove(baseUrl);

        utils::Time::nanoSleep(utils::Time::ms * 1000); // let pending packets be sent and received)

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);
        finalizeSimulation();

        logVideoSent("client1", client1);
        logVideoSent("client2", client2);
        logVideoSent("client3", client3);

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = client1._transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = client1.getAudioReceiveStats();
            std::vector<double> allFreq;
            EXPECT_EQ(rData1.size(), 2);

            for (const auto& item : rData1)
            {
                if (client1.isRemoteVideoSsrc(item.first))
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
            auto videoReceiveStats = client1._transport->getCumulativeVideoReceiveCounters();
            client2._transport->getReportSummary(transportSummary2);
            client3._transport->getReportSummary(transportSummary3);

            logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
            logTransportSummary("client2", client2._transport.get(), transportSummary2);
            logTransportSummary("client3", client3._transport.get(), transportSummary3);

            EXPECT_NEAR(videoReceiveStats.packets,
                transportSummary2.begin()->second.packetsSent + transportSummary3.begin()->second.packetsSent,
                25);
        }
        {
            auto audioCounters = client2._transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData1 = client2.getAudioReceiveStats();
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
    runTestInThread(_numWorkerThreads + 4, [this]() {
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
        bridge2->initialize(_endpointFacory);

        startSimulation();

        const auto baseUrl = "http://127.0.0.1:8080";
        const auto baseUrl2 = "http://127.0.0.1:8090";

        Conference conf;
        conf.create(baseUrl);
        EXPECT_TRUE(conf.isSuccess());

        Conference conf2;
        conf2.create(baseUrl2);
        EXPECT_TRUE(conf2.isSuccess());

        utils::Time::nanoSleep(1 * utils::Time::sec);

        SfuClient<Channel> client1(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);
        SfuClient<Channel> client2(++_instanceCounter,
            *_mainPoolAllocator,
            _audioAllocator,
            *_transportFactory,
            *_sslDtls);

        GroupCall<SfuClient<Channel>> groupCall({&client1, &client2});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl2, conf2.getId(), false, true, true, true);

        if (!groupCall.connect(utils::Time::sec * 8))
        {
            EXPECT_TRUE(false);
            return;
        }

        client1._audioSource->setFrequency(600);
        client2._audioSource->setFrequency(1300);

        client1._audioSource->setVolume(0.6);
        client2._audioSource->setVolume(0.6);

        utils::Time::nanoSleep(500 * utils::Time::ms);

        Barbell bb1;
        Barbell bb2;

        auto sdp1 = bb1.allocate(baseUrl, conf.getId(), true);
        auto sdp2 = bb2.allocate(baseUrl2, conf2.getId(), false);

        bb1.configure(sdp2);
        bb2.configure(sdp1);

        utils::Time::nanoSleep(2 * utils::Time::sec);

        groupCall.run(utils::Time::ms * 5000);

        client2.stopRecording();
        client1.stopRecording();

        HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
        statsRequest.awaitResponse(1500 * utils::Time::ms);
        EXPECT_TRUE(statsRequest.isSuccess());
        HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
        confRequest.awaitResponse(500 * utils::Time::ms);
        EXPECT_TRUE(confRequest.isSuccess());

        utils::Time::nanoSleep(utils::Time::ms * 200); // let pending packets be sent and received
        client1._transport->stop();
        client2._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();

        logVideoSent("client1", client1);
        logVideoSent("client2", client2);

        const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        {
            auto audioCounters = client1._transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);
            const auto& rData1 = client1.getAudioReceiveStats();
            std::vector<double> allFreq;
            EXPECT_EQ(rData1.size(), 1);

            for (const auto& item : rData1)
            {
                if (client1.isRemoteVideoSsrc(item.first))
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
            ASSERT_GE(allFreq.size(), 1);
            EXPECT_NEAR(allFreq[0], 1300.0, 25.0);

            std::unordered_map<uint32_t, transport::ReportSummary> transportSummary2;
            auto videoReceiveStats = client1._transport->getCumulativeVideoReceiveCounters();
            client2._transport->getReportSummary(transportSummary2);

            logger::debug("client1 received video pkts %" PRIu64, "bbTest", videoReceiveStats.packets);
            logTransportSummary("client2", client2._transport.get(), transportSummary2);

            EXPECT_NEAR(videoReceiveStats.packets, transportSummary2.begin()->second.packetsSent, 25);
        }
        {
            auto audioCounters = client2._transport->getCumulativeAudioReceiveCounters();
            EXPECT_EQ(audioCounters.lostPackets, 0);

            const auto& rData1 = client2.getAudioReceiveStats();
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
        }
    });
}

TEST_F(IntegrationTest, detectIsPtt)
{
    runTestInThread(_numWorkerThreads + 4, [this]() {
        _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

        initBridge(_config);

        startSimulation();

        const auto baseUrl = "http://127.0.0.1:8080";

        DEFINE_3_CLIENT_CONFERENCE(Channel, baseUrl)

        GroupCall<SfuClient<Channel>> groupCall({&client1, &client2, &client3});

        client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
        client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
        client3.initiateCall(baseUrl, conf.getId(), false, true, true, true);

        auto connectResult = groupCall.connect(utils::Time::sec * 5);
        ASSERT_TRUE(connectResult);
        if (!connectResult)
        {
            return;
        }

        client1._audioSource->setFrequency(600);
        client2._audioSource->setFrequency(1300);
        client3._audioSource->setFrequency(2100);

        client1._audioSource->setVolume(0.6);
        client2._audioSource->setVolume(0.6);
        client3._audioSource->setVolume(0.6);

        // Disable audio level extension, otherwise constant signal will lead to the 'noise leve' equal to the signal
        // and detection would fail
        client1._audioSource->setUseAudioLevel(false);
        client2._audioSource->setUseAudioLevel(false);
        client3._audioSource->setUseAudioLevel(false);

        // =============================== PART 1: #1 & #2 talking ====================

        client1._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        client2._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        client3._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        groupCall.run(utils::Time::sec * 2);

        auto endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_TRUE(isActiveTalker(endpoints, client1._channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, client2._channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, client3._channel.getEndpointId()));

        auto endpointExtendedInfo = getEndpointExtendedInfo(baseUrl, endpoints[0].id);

        EXPECT_EQ(endpoints[0], endpointExtendedInfo.basicEndpointInfo);
        EXPECT_EQ(10000, endpointExtendedInfo.localPort);

        // We construct pseudo-usid from ssrc, so we can check it here.
        auto expectedUsid = __builtin_bswap32(endpointExtendedInfo.ssrcOriginal << 8);
        EXPECT_TRUE(1 << 24 > endpointExtendedInfo.userId.get());
        EXPECT_EQ(expectedUsid, endpointExtendedInfo.userId.get());
        EXPECT_EQ(endpointExtendedInfo.ssrcOriginal, client1._audioSource->getSsrc());
        EXPECT_NE(endpointExtendedInfo.ssrcOriginal, endpointExtendedInfo.ssrcRewritten);
        EXPECT_TRUE(endpointExtendedInfo.ssrcRewritten != 0);

        // =============================== PART 2: #2 talking =========================

        client1._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        client2._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);
        client3._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        groupCall.run(utils::Time::sec * 2);
        utils::Time::nanoSleep(utils::Time::sec * 1);

        endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, client1._channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, client2._channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, client3._channel.getEndpointId()));

        // =============================== PART 3: #3 talking =========================

        client1._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        client2._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        client3._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Set);

        groupCall.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, client1._channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, client2._channel.getEndpointId()));
        EXPECT_TRUE(isActiveTalker(endpoints, client3._channel.getEndpointId()));

        // =============================== PART 4: nobody talking =====================

        client1._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        client2._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);
        client3._audioSource->setIsPtt(emulator::AudioSource::IsPttState::Unset);

        groupCall.run(utils::Time::sec * 2);

        endpoints = getConferenceEndpointsInfo(baseUrl);
        EXPECT_EQ(3, endpoints.size());

        EXPECT_FALSE(isActiveTalker(endpoints, client1._channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, client2._channel.getEndpointId()));
        EXPECT_FALSE(isActiveTalker(endpoints, client3._channel.getEndpointId()));

        client3.stopRecording();
        client2.stopRecording();
        client1.stopRecording();

        client1._transport->stop();
        client2._transport->stop();
        client3._transport->stop();

        groupCall.awaitPendingJobs(utils::Time::sec * 4);

        finalizeSimulation();
    });
};

#undef DEFINE_3_CLIENT_CONFERENCE
