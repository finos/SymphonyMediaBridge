#include "test/integration/IntegrationTest.h"
#include "api/ConferenceEndpoint.h"
#include "api/Parser.h"
#include "api/utils.h"
#include "bridge/Mixer.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcHashmap.h"
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
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"
#include <complex>
#include <sstream>
#include <unordered_set>

#define DEFINE_3_CLIENT_CONFERENCE(TChannel, BASE_URL)                                                                 \
    Conference conf;                                                                                                   \
    conf.create(BASE_URL);                                                                                             \
    EXPECT_TRUE(conf.isSuccess());                                                                                     \
    utils::Time::nanoSleep(1 * utils::Time::sec);                                                                      \
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
      _pacer(10 * 1000000),
      _instanceCounter(0)
{
    for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }
}

void IntegrationTest::SetUp()
{
    using namespace std;

    utils::Time::initialize();
}

void IntegrationTest::initBridge(config::Config& config)
{
    _bridge = std::make_unique<bridge::Bridge>(config);
    _bridge->initialize();

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
        *_mainPoolAllocator);
}

void IntegrationTest::TearDown()
{
    _bridge.reset();
    _transportFactory.reset();
    _jobManager->stop();
    for (auto& worker : _workerThreads)
    {
        worker->stop();
    }

    logger::info("IntegrationTest torn down", "IntegrationTest");
}

namespace
{
void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId)
{
    utils::RateTracker<5> amplitudeTracker(codec::Opus::sampleRate / 10);
    const size_t fftWindowSize = 2048;
    std::valarray<std::complex<double>> testVector(fftWindowSize);

    for (size_t t = 0; t < recording.size(); ++t)
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

    if (recording.size() < fftWindowSize)
    {
        return;
    }

    for (size_t cursor = 0; cursor < recording.size() - fftWindowSize; cursor += 256)
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
void make5secCallWithDefaultAudioProfileAndStopClient3(SfuClient<TChannel>& client1,
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

    client3._transport->stop();

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

TEST_F(IntegrationTest, plain)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

    initBridge(_config);

    const std::string baseUrl = "http://127.0.0.1:8080";

    DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

    GroupCall<SfuClient<ColibriChannel>> groupCall = {&client1, &client2, &client3};

    client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
    client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
    client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

    make5secCallWithDefaultAudioProfileAndStopClient3(client1, client2, client3, groupCall);

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

    groupCall.awaitPendingJobs(utils::Time::sec * 4);

    const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    {
        auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);
        const auto& rData1 = client1.getReceiveStats();
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

        const auto& rData1 = client2.getReceiveStats();
        std::vector<double> allFreq;
        for (const auto& item : rData1)
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

        const auto& rData1 = client3.getReceiveStats();
        // We expect one audio ssrc, three video and one padding
        EXPECT_EQ(rData1.size(), 4);
        size_t audioSsrcCount = 0;
        for (const auto& item : rData1)
        {
            if (client3.isRemoteVideoSsrc(item.first))
            {
                continue;
            }

            ++audioSsrcCount;

            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());

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
            // ramp-up to about 1826 (+-250) in 1.67s (+-0,2s)
            if (amplitudeProfile.size() >= 2)
            {
                EXPECT_EQ(amplitudeProfile[0].second, 0);

                EXPECT_NEAR(amplitudeProfile.back().second, 1826, 250);
                EXPECT_NEAR(amplitudeProfile.back().first, 48000 * 1.67, 48000 * 0.2);
            }

            // item.second->dumpPcmData();
        }

        EXPECT_EQ(audioSsrcCount, 1);
    }
}

TEST_F(IntegrationTest, audioOnlyNoPadding)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString("{\"ip\":\"127.0.0.1\", "
                           "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
    initBridge(_config);

    const std::string baseUrl = "http://127.0.0.1:8080";

    DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

    GroupCall<SfuClient<ColibriChannel>> groupCall({&client1, &client2, &client3});

    // Audio only for all three participants.
    client1.initiateCall(baseUrl, conf.getId(), true, true, false, false);
    client2.initiateCall(baseUrl, conf.getId(), false, true, false, false);
    client3.initiateCall(baseUrl, conf.getId(), false, true, false, false);

    if (!groupCall.connect(utils::Time::sec * 8))
    {
        EXPECT_TRUE(false);
        return;
    }

    client1._audioSource->setFrequency(600);
    client2._audioSource->setFrequency(1300);
    client3._audioSource->setFrequency(2100);

    client1._audioSource->setVolume(0.6);
    client2._audioSource->setVolume(0.6);
    client3._audioSource->setVolume(0.6);

    utils::Pacer pacer(10 * utils::Time::ms);
    for (int i = 0; i < 500; ++i)
    {
        const auto timestamp = utils::Time::getAbsoluteTime();
        client1.process(timestamp, false);
        client2.process(timestamp, false);
        client3.process(timestamp, false);
        pacer.tick(utils::Time::getAbsoluteTime());
        utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
    }

    client3.stopRecording();
    client2.stopRecording();
    client1.stopRecording();

    client1._transport->stop();
    client2._transport->stop();
    client3._transport->stop();

    for (int i = 0; i < 10 &&
         (client1._transport->hasPendingJobs() || client2._transport->hasPendingJobs() ||
             client3._transport->hasPendingJobs());
         ++i)
    {
        utils::Time::nanoSleep(1 * utils::Time::sec);
    }
    const auto& rData1 = client1.getReceiveStats();
    const auto& rData2 = client2.getReceiveStats();
    const auto& rData3 = client3.getReceiveStats();
    // We expect only one ssrc (audio), since padding (that comes on video ssrc) is disabled for audio only
    // calls).
    EXPECT_EQ(rData1.size(), 1);
    EXPECT_EQ(rData2.size(), 1);
    EXPECT_EQ(rData3.size(), 1);
}

TEST_F(IntegrationTest, videoOffPaddingOff)
{
    /*
       Test checks that after video is off and cooldown interval passed, no padding will be sent for the
       call that became audio-only.
    */
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(
        "{\"ip\":\"127.0.0.1\", "
        "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\", \"rctl.cooldownInterval\":1}");
    initBridge(_config);

    const std::string baseUrl = "http://127.0.0.1:8080";

    DEFINE_3_CLIENT_CONFERENCE(ColibriChannel, baseUrl + "/colibri")

    GroupCall<SfuClient<ColibriChannel>> groupCall = {&client1, &client2};

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

    // Have to produce some audio volume above "silence threshold", otherwise audio packats
    // won't be forwarded by SFU.
    client1._audioSource->setFrequency(600);
    client2._audioSource->setFrequency(1300);

    client1._audioSource->setVolume(0.6);
    client2._audioSource->setVolume(0.6);

    utils::Pacer pacer(10 * utils::Time::ms);
    for (int i = 0; i < 100; ++i)
    {
        const auto timestamp = utils::Time::getAbsoluteTime();
        client1.process(timestamp, false);
        client2.process(timestamp, true);
        pacer.tick(utils::Time::getAbsoluteTime());
        utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
    }

    client2.stopRecording();
    client2._transport->stop();

    const auto numSsrcClient1Received = client1.getReceiveStats().size();

    // Video producer (client2) stopped, waiting 1.5s for rctl.cooldownInterval timeout to take effect
    // (configured for 1 s for this test).

    for (int i = 0; i < 150; ++i)
    {
        const auto timestamp = utils::Time::getAbsoluteTime();
        client1.process(timestamp, false);
        utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
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
        pacer.tick(utils::Time::getAbsoluteTime());
        utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
    }

    client1.stopRecording();
    client3.stopRecording();

    client1._transport->stop();
    client3._transport->stop();

    for (int i = 0; i < 10 &&
         (client1._transport->hasPendingJobs() || client2._transport->hasPendingJobs() ||
             client3._transport->hasPendingJobs());
         ++i)
    {
        utils::Time::nanoSleep(1 * utils::Time::sec);
    }

    const auto& rData1 = client1.getReceiveStats();
    const auto& rData2 = client2.getReceiveStats();
    const auto& rData3 = client3.getReceiveStats();

    EXPECT_EQ(numSsrcClient1Received, 3); // s2's audio, s2's video + padding
    EXPECT_EQ(rData1.size(), 4); // s2's audio, s3's audio, s2's video + padding
    EXPECT_EQ(rData2.size(), 2); // s1's aidio, + padding
    EXPECT_EQ(rData3.size(),
        1); // s1's aidio, no padding, since it joined the call whith no video.  !!!TODO config timeout
}

TEST_F(IntegrationTest, plainNewApi)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

    initBridge(_config);

    const auto baseUrl = "http://127.0.0.1:8080";

    Conference conf;
    conf.create(baseUrl);
    EXPECT_TRUE(conf.isSuccess());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    SfuClient<Channel> client1(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);
    SfuClient<Channel> client2(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);
    SfuClient<Channel> client3(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);

    GroupCall<SfuClient<Channel>> groupCall = {&client1, &client2, &client3};

    client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
    client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
    client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

    if (!groupCall.connect(utils::Time::sec * 8))
    {
        EXPECT_TRUE(false);
        return;
    }

    client1._audioSource->setFrequency(600);
    client2._audioSource->setFrequency(1300);
    client3._audioSource->setFrequency(2100);

    client1._audioSource->setVolume(0.6);
    client2._audioSource->setVolume(0.6);
    client3._audioSource->setVolume(0.6);

    groupCall.run(utils::Time::ms * 5000);

    client3._transport->stop();

    client3.stopRecording();
    client2.stopRecording();
    client1.stopRecording();

    HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
    statsRequest.awaitResponse(1500 * utils::Time::ms);
    EXPECT_TRUE(statsRequest.isSuccess());
    HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    client1._transport->stop();
    client2._transport->stop();

    groupCall.awaitPendingJobs(utils::Time::sec * 4);

    const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    {
        auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);
        const auto& rData1 = client1.getReceiveStats();
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

        const auto& rData1 = client2.getReceiveStats();
        std::vector<double> allFreq;
        for (const auto& item : rData1)
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

        const auto& rData1 = client3.getReceiveStats();
        // We expect one audio ssrc and 1 video (where we receive padding data due to RateController)
        EXPECT_EQ(rData1.size(), 4);
        size_t audioSsrcCount = 0;
        for (const auto& item : rData1)
        {
            if (client3.isRemoteVideoSsrc(item.first))
            {
                continue;
            }

            ++audioSsrcCount;

            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());

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
            // ramp-up to about 1826 (+-250) in 1.67s (+-0,2s)
            if (amplitudeProfile.size() >= 2)
            {
                EXPECT_EQ(amplitudeProfile[0].second, 0);

                EXPECT_NEAR(amplitudeProfile.back().second, 1826, 250);
                EXPECT_NEAR(amplitudeProfile.back().first, 48000 * 1.67, 48000 * 0.2);
            }

            // item.second->dumpPcmData();
        }

        EXPECT_EQ(audioSsrcCount, 1);
    }
}

TEST_F(IntegrationTest, ptime10)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

    initBridge(_config);

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
    SfuClient<Channel> client2(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);
    SfuClient<Channel> client3(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);

    GroupCall<SfuClient<Channel>> groupCall = {&client1, &client2, &client3};

    client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
    client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
    client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

    make5secCallWithDefaultAudioProfileAndStopClient3(client1, client2, client3, groupCall);

    HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
    statsRequest.awaitResponse(1500 * utils::Time::ms);
    EXPECT_TRUE(statsRequest.isSuccess());
    HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    client1._transport->stop();
    client2._transport->stop();

    groupCall.awaitPendingJobs(utils::Time::sec * 4);

    const auto audioPacketSampleCount = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    {
        auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);
        const auto& rData1 = client1.getReceiveStats();
        std::vector<double> allFreq;
        EXPECT_EQ(rData1.size(), 5);

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

        const auto& rData1 = client2.getReceiveStats();
        EXPECT_EQ(rData1.size(), 5);
        std::vector<double> allFreq;
        for (const auto& item : rData1)
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

        const auto& rData1 = client3.getReceiveStats();
        // We expect one audio ssrc and 1 video (where we receive padding data due to RateController)
        EXPECT_EQ(rData1.size(), 4);
        size_t audioSsrcCount = 0;
        for (const auto& item : rData1)
        {
            if (client3.isRemoteVideoSsrc(item.first))
            {
                continue;
            }

            ++audioSsrcCount;

            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());

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
            // ramp-up to about 1826 (+-250) in 1.67s (+-0,2s)
            if (amplitudeProfile.size() >= 2)
            {
                EXPECT_EQ(amplitudeProfile[0].second, 0);

                EXPECT_NEAR(amplitudeProfile.back().second, 1826, 250);
                EXPECT_NEAR(amplitudeProfile.back().first, 48000 * 1.67, 48000 * 0.2);
            }

            // item.second->dumpPcmData();
        }

        EXPECT_EQ(audioSsrcCount, 1);
    }
}

TEST_F(IntegrationTest, simpleBarbell)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

    initBridge(_config);

    config::Config config2;
    config2.readFromString(
        R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "ice.singlePort":12000,
        "port":8090,
        "recording.singlePort":12500
        })");

    auto bridge2 = std::make_unique<bridge::Bridge>(config2);
    bridge2->initialize();

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

    utils::Time::nanoSleep(5 * utils::Time::sec);

    SfuClient<Channel> client1(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);
    SfuClient<Channel> client2(++_instanceCounter, *_mainPoolAllocator, _audioAllocator, *_transportFactory, *_sslDtls);

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

    groupCall.run(utils::Time::ms * 5000);

    client2.stopRecording();
    client1.stopRecording();

    HttpGetRequest statsRequest((std::string(baseUrl) + "/stats").c_str());
    statsRequest.awaitResponse(1500 * utils::Time::ms);
    EXPECT_TRUE(statsRequest.isSuccess());
    HttpGetRequest confRequest((std::string(baseUrl) + "/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    client1._transport->stop();
    client2._transport->stop();

    groupCall.awaitPendingJobs(utils::Time::sec * 4);
}

TEST_F(IntegrationTest, detectIsPtt)
{
    if (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    {
        return;
    }
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1"
        })");

    initBridge(_config);

    const auto baseUrl = "http://127.0.0.1:8080";

    DEFINE_3_CLIENT_CONFERENCE(Channel, baseUrl)

    GroupCall<SfuClient<Channel>> groupCall = {&client1, &client2, &client3};

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

    // Disable audio level extension, otherwise constant signal will lead to the 'noise leve' equal to the signal and
    // detection would fail
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
}

#undef DEFINE_3_CLIENT_CONFERENCE
