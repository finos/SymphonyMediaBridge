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
void makeCallWithDefaultAudioProfile(GroupCall<SfuClient<TChannel>>& groupCall, uint64_t duration)
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

    groupCall.run(duration);
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

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        auto audio = createTalker && count++ < numSpeakers ? emulator::Audio::Opus : emulator::Audio::Muted;
        client->initiateCall(baseUrl.c_str(), conf.getId(), true, audio, false, true);
        client->setExpectedAudioType(Audio::Opus);
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
    makeCallWithDefaultAudioProfile(group, duration * utils::Time::sec);

    group.disconnectClients();
    group.stopTransports();

    group.awaitPendingJobs(utils::Time::sec * 4);

    IntegrationTest::AudioAnalysisData results[numClients];
    for (size_t id = 0; id < numClients; ++id)
    {
        auto durationForAnalizys = std::min(duration, (decltype(duration))15);
        results[id] =
            IntegrationTest::analyzeRecording<SfuClient<Channel>>(group.clients[id].get(), durationForAnalizys, false);

        std::unordered_map<uint32_t, transport::ReportSummary> transportSummary;
        std::string clientName = "client_" + std::to_string(id);
        group.clients[id]->_transport->getReportSummary(transportSummary);
        logTransportSummary(clientName.c_str(), group.clients[id]->_transport.get(), transportSummary);
    }

    for (size_t id = 0; id < numClients; ++id)
    {
        const auto expectedChannelsReceived = id < numSpeakers ? numSpeakers - 1 : numSpeakers;
        if (expectedChannelsReceived)
        {
            std::unordered_set<double> received;
            for (const auto& freq : results[id].dominantFrequencies)
            {
                received.insert(freq);

                auto receivedBytes = results[id].receivedBytes[freq];
                EXPECT_NEAR(receivedBytes,
                    codec::Opus::sampleRate * duration,
                    codec::Opus::sampleRate * duration * 0.05);
                logger::info("Client %zu received: %zu bytes for freq %f", "RealTimeTest", id, receivedBytes, freq);
            }

            EXPECT_EQ(results[id].dominantFrequencies.size(), expectedChannelsReceived);
            for (const auto& freq : results[id].dominantFrequencies)
            {
                for (size_t i = 0; i < numSpeakers; i++)
                {
                    // We should not receive our own audio
                    if (i == id)
                    {
                        continue;
                    }
                    if (freq > RealTimeTest::frequencies[i] * 0.9 && freq < RealTimeTest::frequencies[i] * 1.1)
                    {
                        received.erase(freq);
                    }
                }
            }
            EXPECT_EQ(received.size(), 0);
            if (received.size())
            {
                for (const auto& item : received)
                {
                    logger::error("Unexpected frequency %f", "RealTimeTest", item);
                }
            }
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

    GroupCall<SfuClient<Channel>>
        group(nullptr, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_clientTransportFactory, *_sslDtls, 50);

    Conference conf(nullptr);

    group.startConference(conf, baseUrl);

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        if (count++ == 0)
        {
            client->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Fake, false, true);
        }
        else
        {
            client->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Muted, false, true);
        }
    }

    ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

    makeCallWithDefaultAudioProfile(group, 20 * utils::Time::sec);

    group.disconnectClients();

    for (auto& client : group.clients)
    {
        client->_transport->stop();
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

    GroupCall<SfuClient<Channel>>
        group(nullptr, _instanceCounter, *_mainPoolAllocator, _audioAllocator, *_clientTransportFactory, *_sslDtls, 3);

    Conference conf(nullptr);

    group.startConference(conf, baseUrl);

    uint32_t count = 0;
    for (auto& client : group.clients)
    {
        if (count++ == 0)
        {
            client->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Fake, true, true);
        }
        else
        {
            client->initiateCall(baseUrl, conf.getId(), true, emulator::Audio::Fake, count < 11, true);
        }
    }

    ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

    makeCallWithDefaultAudioProfile(group, 20 * utils::Time::sec);

    group.disconnectClients();

    for (auto& client : group.clients)
    {
        client->_transport->stop();
    }

    group.awaitPendingJobs(utils::Time::sec * 4);
}
