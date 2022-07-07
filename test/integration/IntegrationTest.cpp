#include "test/integration/IntegrationTest.h"
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

class HttpPostRequest
{
public:
    HttpPostRequest(const char* url, const char* body) : _request(nullptr), _status(HTTP_STATUS_PENDING), _prevSize(0)
    {
        _request = http_post(url, body, body ? std::strlen(body) : 0, nullptr);
    }

    ~HttpPostRequest() { http_release(_request); }

    void awaitResponse(uint64_t timeout)
    {
        const auto startTime = utils::Time::getAbsoluteTime();

        while (_status == HTTP_STATUS_PENDING)
        {
            _status = http_process(_request);
            if (_prevSize != _request->response_size)
            {
                logger::debug("%zu byte(s) received.", "HttpPostRequest", _request->response_size);
                _prevSize = _request->response_size;
            }
            if (utils::Time::getAbsoluteTime() - startTime > timeout)
            {
                logger::error("Timeout waiting for response", "HttpPostRequest");
                _status = HTTP_STATUS_FAILED;
                break;
            }
            utils::Time::nanoSleep(2 * utils::Time::ms);
        }
    }

    bool isPending() const { return _status == HTTP_STATUS_PENDING; }
    bool hasFailed() const { return _status == HTTP_STATUS_FAILED; }
    bool isSuccess() const { return _status == HTTP_STATUS_COMPLETED; }

    std::string getResponse() const
    {
        if (isSuccess())
        {
            return (char const*)_request->response_data;
        }
        return "";
    }

    nlohmann::json getJsonBody() const
    {
        if (isSuccess())
        {
            return nlohmann::json::parse(static_cast<const char*>(_request->response_data));
        }
        return nlohmann::json();
    }

    int getCode() const { return _request->status_code; }

protected:
    HttpPostRequest() : _request(nullptr), _status(HTTP_STATUS_PENDING), _prevSize(0) {}
    http_t* _request;

private:
    http_status_t _status;
    size_t _prevSize;
};

class HttpPatchRequest : public HttpPostRequest
{
public:
    HttpPatchRequest(const char* url, const char* body)
    {
        _request = http_patch(url, body, body ? std::strlen(body) : 0, nullptr);
    }
};

class HttpGetRequest : public HttpPostRequest
{
public:
    HttpGetRequest(const char* url) { _request = http_get(url, nullptr); }
};

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

TEST_F(IntegrationTest, plain)
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

    Conference conf;
    conf.create(baseUrl + "/colibri");
    EXPECT_TRUE(conf.isSuccess());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    SfuClient<ColibriChannel> client1(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client2(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client3(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);

    GroupCall<SfuClient<ColibriChannel>> groupCall = {&client1, &client2, &client3};

    client1.initiateCall(baseUrl, conf.getId(), true, true, true, true);
    client2.initiateCall(baseUrl, conf.getId(), false, true, true, true);
    client3.initiateCall(baseUrl, conf.getId(), false, true, true, false);

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

    HttpGetRequest statsRequest((std::string(baseUrl) + "/colibri/stats").c_str());
    statsRequest.awaitResponse(1500 * utils::Time::ms);
    EXPECT_TRUE(statsRequest.isSuccess());
    HttpGetRequest confRequest((std::string(baseUrl) + "/colibri/conferences").c_str());
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

    Conference conf;
    conf.create(baseUrl + "/colibri");
    EXPECT_TRUE(conf.isSuccess());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    SfuClient<ColibriChannel> client1(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client2(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client3(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);

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
    // We expect only one ssrc (audio), since padding (that comes on video ssrc) is disabled for audio only calls).
    EXPECT_EQ(rData1.size(), 1);
    EXPECT_EQ(rData2.size(), 1);
    EXPECT_EQ(rData3.size(), 1);
}

TEST_F(IntegrationTest, videoOffPaddingOff)
{
    /*
       Test checks that after video is off and cooldown interval passed, no padding will be sent for the call that
       became audio-only.
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

    Conference conf;
    conf.create(baseUrl + "/colibri");
    EXPECT_TRUE(conf.isSuccess());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    SfuClient<ColibriChannel> client1(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client2(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);
    SfuClient<ColibriChannel> client3(++_instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_transportFactory,
        *_sslDtls);

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

    _config.readFromString("{\"ip\":\"127.0.0.1\", "
                           "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
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

    _config.readFromString("{\"ip\":\"127.0.0.1\", "
                           "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
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

    if (!groupCall.connect(utils::Time::sec * 4))
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

    groupCall.run(utils::Time::sec * 5);

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
