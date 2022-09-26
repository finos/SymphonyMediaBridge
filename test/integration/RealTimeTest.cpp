#include "test/integration/RealTimeTest.h"
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
#include "utils/StringBuilder.h"
#include <chrono>
#include <complex>
#include <memory>
#include <sstream>
#include <unordered_set>

#define USE_FAKENETWORK 1

RealTimeTest::RealTimeTest()
    : _sendAllocator(memory::packetPoolSize * 8, "RealTimeTest"),
      _audioAllocator(memory::packetPoolSize * 8, "RealTimeTestAudio"),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(memory::packetPoolSize * 32, "testMain")),
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
void RealTimeTest::SetUp()
{
#ifdef NOPERF_TEST
    // GTEST_SKIP();
#endif
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;

    _jobManager = std::make_unique<jobmanager::JobManager>();
    for (size_t threadIndex = 0; threadIndex < getNumWorkerThreads(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
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
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_clientConfig.ice.preferredIp, 0));

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

template <typename TChannel>
void makeCallWithDefaultAudioProfile(GroupCall<SfuClient<TChannel>>& groupCall, uint64_t duration)
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

TEST_F(RealTimeTest, megaHoot)
{
    // GTEST_SKIP();
    utils::Time::initialize(); // run in real time
    _bridgeConfig.readFromString(R"({
        "ip":"127.0.0.1",
        "ice.preferredIp":"127.0.0.1",
        "ice.publicIpv4":"127.0.0.1",
        "log.level": "INFO"
        })");

    // initRealBridge(_config);

    const auto baseUrl = "http://127.0.0.1:8080";

    GroupCall<SfuClient<Channel>> group(nullptr,
        _instanceCounter,
        *_mainPoolAllocator,
        _audioAllocator,
        *_clientTransportFactory,
        *_sslDtls,
        1000);

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

    for (auto& client : group.clients)
    {
        client->_transport->stop();
    }

    group.awaitPendingJobs(utils::Time::sec * 4);
}
