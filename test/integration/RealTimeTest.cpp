#include "test/integration/RealTimeTest.h"
#include "IntegrationTest.h"
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
#include "utils/MersienneRandom.h"
#include "utils/StringBuilder.h"
#include <algorithm>
#include <chrono>
#include <complex>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace config
{
const char* g_LoadTestConfigFile = nullptr;
}

RealTimeTest::RealTimeTest()
    : _sendAllocator(memory::packetPoolSize * 8, "RealTimeTest"),
      _audioAllocator(memory::packetPoolSize * 8, "RealTimeTestAudio"),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(memory::packetPoolSize * 32, "testMain")),
      _sslDtls(nullptr),
      _network(transport::createRtcePoll()),
      _pacer(10 * utils::Time::ms),
      _instanceCounter(0),
      _numWorkerThreads(getNumWorkerThreads()),
      _clientsConnectionTimeout(15),
      _config(std::make_unique<config::LoadTestConfig>()),
      _configInitialized(false)
{
    if (config::g_LoadTestConfigFile != nullptr)
    {
        _configInitialized = _config->readFromFile(config::g_LoadTestConfigFile);
        if (!_configInitialized)
        {
            logger::error("Failed to read load test configuration from %s",
                "RealTimeTest",
                config::g_LoadTestConfigFile);
        }
    }
    else
    {
        logger::info("No load test configuration provided, load test will fail.", "RealTimeTest");
    }
}

// TimeTurner time source must be set before starting any threads.
// Fake internet thread, JobManager timer thread, worker threads.
void RealTimeTest::SetUp()
{
#ifdef NOPERF_TEST
    // GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;
    utils::Time::initialize(); // run in real time
    _timerQueue = std::make_unique<jobmanager::TimerQueue>(4096);
    _jobManager = std::make_unique<jobmanager::JobManager>(*_timerQueue);
    for (size_t threadIndex = 0; threadIndex < getNumWorkerThreads(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager, true));
    }

    initLocalTransports();
}

void RealTimeTest::TearDown()
{
#ifdef NOPERF_TEST
    // GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    _bridge.reset();
    _clientTransportFactory.reset();
    _timerQueue->stop();
    _jobManager->stop();
    for (auto& worker : _workerThreads)
    {
        worker->stop();
    }

    logger::info("RealTimeTest torn down", "RealTimeTest");
}

size_t RealTimeTest::getNumWorkerThreads() const
{
    const auto hardwareConcurrency = std::thread::hardware_concurrency();
    if (hardwareConcurrency == 0)
    {
        return 7;
    }
    return std::max(hardwareConcurrency - 1, 1U);
}

void RealTimeTest::initRealBridge(config::Config& config)
{
    _bridge = std::make_unique<bridge::Bridge>(config);
    _bridge->initialize();
}

void RealTimeTest::initLocalTransports()
{
    _sslDtls = std::make_unique<transport::SslDtls>();
    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);

    std::string configJson =
        "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10050, \"recording.singlePort\":0}";
    _clientConfig.readFromString(configJson);
    std::vector<transport::SocketAddress> interfaces = bridge::gatherInterfaces(_clientConfig);

    _clientsEndpointFactory = std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl());
    _clientTransportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _clientConfig,
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

const double RealTimeTest::frequencies[] = {600, 1300, 2100, 3200, 4100, 4800, 5200};

template <typename TChannel>
void makeCallWithDefaultAudioProfile(GroupCall<SfuClient<TChannel>>& groupCall,
    uint64_t duration,
    size_t firstNclientsToModulate = 0)
{
    for (size_t i = 0; i < groupCall.clients.size(); ++i)
    {
        groupCall.clients[i]->_audioSource->setFrequency(
            RealTimeTest::frequencies[i % (sizeof(RealTimeTest::frequencies) / sizeof(RealTimeTest::frequencies[0]))]);
    }

    for (auto& client : groupCall.clients)
    {
        client->_audioSource->setVolume(0.6);
    }

    groupCall.run(duration, firstNclientsToModulate);
    utils::Time::nanoSleep(utils::Time::sec * 1);

    for (auto& client : groupCall.clients)
    {
        client->stopRecording();
    }
}

bool RealTimeTest::isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint)
{
    auto it = std::find_if(endpoints.cbegin(), endpoints.cend(), [&endpoint](const api::ConferenceEndpoint& e) {
        return e.id == endpoint;
    });
    assert(it != endpoints.cend());
    return it->isActiveTalker;
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot)
{
    RealTimeTest::smbMegaHootTest(1);
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot_1)
{
    RealTimeTest::smbMegaHootTest(1);
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot_2)
{
    RealTimeTest::smbMegaHootTest(2);
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot_3)
{
    RealTimeTest::smbMegaHootTest(3);
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot_4)
{
    RealTimeTest::smbMegaHootTest(4);
}

TEST_F(RealTimeTest, DISABLED_smbMegaHoot_5)
{
    RealTimeTest::smbMegaHootTest(5);
}

template <typename TClient>
RealTimeTest::AudioAnalysisData RealTimeTest::analyzeRecordingSimple(TClient* client,
    double expectedDurationSeconds,
    size_t expectedLossInPackets)
{
    auto audioCounters = client->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
    EXPECT_LE(audioCounters.lostPackets, expectedLossInPackets);

    const auto& data = client->getAudioReceiveStats();
    RealTimeTest::AudioAnalysisData result;

    for (const auto& item : data)
    {
        if (client->isRemoteVideoSsrc(item.first))
        {
            continue;
        }

        result.audioSsrcCount++;
        auto rec = item.second->getRecording();
        result.receivedBytes[item.first] = rec.size();
    }

    return result;
}

void RealTimeTest::smbMegaHootTest(const size_t numSpeakers)
{
    std::string baseUrl = "http://127.0.0.1:8080";
    size_t numClients = 40;
    bool createTalker = true;
    uint16_t duration = 15;
    uint32_t rampup = 5;
    uint32_t max_rampup = 10;
    utils::MersienneRandom<uint32_t> randGen;

    if (_configInitialized)
    {
        numClients = _config->numClients;
        duration = _config->duration;
        max_rampup = _config->max_rampup;
        rampup = max_rampup ? randGen.next() % max_rampup : 0;

        utils::StringBuilder<1000> sb;
        sb.append("http://");
        sb.append(_config->ip.get().empty() ? _config->address : _config->ip);
        sb.append(":");
        sb.append(_config->port);
        baseUrl = sb.build();
        createTalker = _config->initiator.get();

        assert(!_config->conference_id.get().empty());
    }

    logger::info("Starting smbMegaHoot test:\n\tbaseUrl: %s\n\tnumClients: %zu",
        "RealTimeTest",
        baseUrl.c_str(),
        numClients);

    GroupCall<SfuClient<Channel>> group(nullptr,
        _instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_clientTransportFactory,
        *_clientTransportFactory,
        *_sslDtls,
        numClients);

    Conference conf(nullptr);

    if (_configInitialized)
    {
        conf.createFromExternal(_config->conference_id.get());
    }
    else if (!group.startConference(conf, baseUrl.c_str()))
    {
        return;
    }

    logger::info("SYNC: Waiting before join for: %d s, start after: %d s", "RealTimeTest", rampup, max_rampup);

    auto startTime = utils::Time::getAbsoluteTime();
    if (rampup != 0)
    {
        utils::Time::nanoSleep(rampup * utils::Time::sec);
    }

    CallConfigBuilder cfg(conf.getId());
    cfg.url(baseUrl).withOpus();

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        if (!createTalker || count == numSpeakers)
        {
            cfg.muted();
        }

        if (count == 0)
        {
            client->initiateCall(cfg.build());
        }
        else
        {
            client->joinCall(cfg.build());
        }
        client->setExpectedAudioType(Audio::Opus);
        ++count;
    }

    ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

    const auto now = utils::Time::getAbsoluteTime();
    const auto diff = utils::Time::diff(startTime, now);
    const auto waitForMore = utils::Time::diff(diff, max_rampup * utils::Time::sec);

    if (max_rampup != 0 && waitForMore > 0)
    {
        logger::info("Waiting before start for another: %" PRId64 "s", "RealTimeTest", waitForMore / utils::Time::sec);
        utils::Time::nanoSleep(waitForMore);
    }

    logger::info("SYNC: starting audio", "RealTimeTest");
    makeCallWithDefaultAudioProfile(group, duration * utils::Time::sec, createTalker ? numSpeakers : 0);

    group.disconnectClients();
    group.stopTransports();

    group.awaitPendingJobs(utils::Time::sec * 4);

    RealTimeTest::AudioAnalysisData results[numClients];
    for (size_t id = 0; id < numClients; ++id)
    {
        results[id] =
            RealTimeTest::analyzeRecordingSimple<SfuClient<Channel>>(group.clients[id].get(), duration, (size_t)0);

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
        std::string clientName = "client_" + std::to_string(id);
        group.clients[id]->getReportSummary(transportSummary);
        logTransportSummary(clientName.c_str(), transportSummary);
    }

    for (size_t id = 0; id < numClients; ++id)
    {
        const auto expectedChannelsReceived = createTalker && (id < numSpeakers) ? numSpeakers - 1 : numSpeakers;
        if (expectedChannelsReceived)
        {
            size_t receivedBytesTotal = 0;
            for (const auto& receivedBytesPair : results[id].receivedBytes)
            {
                receivedBytesTotal += receivedBytesPair.second;
                logger::info("Client %zu received: %zu bytes from ssrc %zu",
                    "RealTimeTest",
                    id,
                    receivedBytesPair.second,
                    receivedBytesPair.first);
            }

            auto receivedBytesPerProducer = receivedBytesTotal / expectedChannelsReceived;
            EXPECT_NEAR(receivedBytesPerProducer,
                codec::Opus::sampleRate * duration,
                codec::Opus::sampleRate * duration * 0.05);
        }
    }
}

TEST_F(RealTimeTest, DISABLED_localMiniHoot)
{
    _bridgeConfig.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "log.level": "INFO"
        })");

    initRealBridge(_bridgeConfig);

    const auto baseUrl = "http://127.0.0.1:8080";

    GroupCall<SfuClient<Channel>> group(nullptr,
        _instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_clientTransportFactory,
        *_clientTransportFactory,
        *_sslDtls,
        50);

    Conference conf(nullptr);

    group.startConference(conf, baseUrl);
    CallConfigBuilder cfg(conf.getId());
    cfg.url(baseUrl).withAudio();

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        if (count++ == 0)
        {
            client->initiateCall(cfg.withAudio().build());
        }
        else
        {
            client->joinCall(cfg.muted().build());
        }
    }

    ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

    makeCallWithDefaultAudioProfile(group, 20 * utils::Time::sec);

    group.disconnectClients();

    for (auto& client : group.clients)
    {
        client->stopTransports();
    }

    group.awaitPendingJobs(utils::Time::sec * 4);
}

TEST_F(RealTimeTest, localVideoMeeting)
{
    _bridgeConfig.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "log.level": "INFO"
        })");

    initRealBridge(_bridgeConfig);

    const auto baseUrl = "http://127.0.0.1:8080";

    GroupCall<SfuClient<Channel>> group(nullptr,
        _instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_clientTransportFactory,
        *_clientTransportFactory,
        *_sslDtls,
        3);

    Conference conf(nullptr);

    [[maybe_unused]] auto confOk = group.startConference(conf, baseUrl);
    assert(confOk);
    CallConfigBuilder cfg(conf.getId());
    cfg.url(baseUrl).withAudio().withVideo();

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        if (count++ == 0)
        {
            client->initiateCall(cfg.build());
        }
        else
        {
            if (count == 11)
            {
                cfg.noVideo();
            }
            client->joinCall(cfg.build());
        }
    }

    ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

    makeCallWithDefaultAudioProfile(group, 20 * utils::Time::sec);

    nlohmann::json responseBody;
    auto briefConfRequest = emulator::awaitResponse<HttpGetRequest>(nullptr,
        std::string(baseUrl) + "/conferences?brief=1",
        1500 * utils::Time::ms,
        responseBody);
    logger::info("%s", "test", responseBody.dump(3).c_str());
    EXPECT_TRUE(responseBody.size() == 1);
    EXPECT_TRUE(briefConfRequest);
    auto& mixerJson = responseBody[0];
    EXPECT_EQ(mixerJson["id"], conf.getId());
    EXPECT_EQ(mixerJson["usercount"], group.clients.size());

    briefConfRequest = emulator::awaitResponse<HttpGetRequest>(nullptr,
        std::string(baseUrl) + "/conferences?brief",
        1500 * utils::Time::ms,
        responseBody);
    logger::info("%s", "test", responseBody.dump(3).c_str());
    EXPECT_TRUE(briefConfRequest);
    EXPECT_TRUE(responseBody.size() == 1);

    group.disconnectClients();

    for (auto& client : group.clients)
    {
        client->stopTransports();
    }

    group.awaitPendingJobs(utils::Time::sec * 4);
}
