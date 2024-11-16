#include "test/integration/IntegrationTest.h"
#include "api/Parser.h"
#include "emulator/FakeEndpointFactory.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "test/integration/emulator/HttpRequests.h"
#include "test/integration/emulator/Httpd.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "utils/Format.h"
#include <gtest/gtest.h>

uint64_t IntegrationTest::AudioAnalysisData::rampupAbove(double amplitude) const
{
    for (auto& item : amplitudeProfile)
    {
        if (item.second >= amplitude)
        {
            return item.first;
        }
    }

    return std::numeric_limits<uint32_t>::max();
}

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
      _ipv4({"192.168.0.11", "35.240.205.93", "31.208.187.140"}),
      _ipv6({"fc00:1208:1208:1208:1208:1208:1208:1100",
          "2600:1900:4080:1627:0:22:0:0",
          "fc00:1808:1808:1808:1808:1808:1808:1100"}),
      _clientsConnectionTimeout(15),
      _networkTickInterval(100 * utils::Time::us)
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
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    using namespace std;

    _smbInterfaces.push_back(transport::SocketAddress::parse(_ipv4.smb));
    _smbInterfaces.push_back(transport::SocketAddress::parse(_ipv6.smb));
    _defaultSmbConfig = utils::format(R"({
        "ip":"127.0.0.1",
        "ice.publicIpv4":"%s",
        "ice.tcp.enable":false,
        "ice.enableIpv6":true
        })",
        _ipv4.smb.c_str());

    utils::Time::initialize(_timeSource);
    _httpd = new emulator::HttpdFactory();
    _internet = std::make_unique<fakenet::InternetRunner>(_networkTickInterval);
    _firewall = std::make_shared<fakenet::Firewall>(transport::SocketAddress::parse(_ipv4.firewall),
        transport::SocketAddress::parse(_ipv6.firewall),
        *_internet->getNetwork());

    _timers = std::make_unique<jobmanager::TimerQueue>(4096);
    _jobManager = std::make_unique<jobmanager::JobManager>(*_timers);
    for (size_t threadIndex = 0; threadIndex < getNumWorkerThreads(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager, true));
    }
}

void IntegrationTest::TearDown()
{
#if !ENABLE_LEGACY_API
    GTEST_SKIP();
#endif

    // if test ran, it will have re initialized, otherwise it is only threads started in Setup that runs.
    if (!utils::Time::isDefaultTimeSource())
    {
        _timeSource.waitForThreadsToSleep(_workerThreads.size() + 2, 3 * utils::Time::sec);
        utils::Time::initialize();
    }
    _timeSource.shutdown();

    _bridge.reset();
    _clientTransportFactory.reset();
    _publicTransportFactory.reset();
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
    _clientsEndpointFactory = std::shared_ptr<transport::EndpointFactory>(new emulator::FakeEndpointFactory(_firewall,
        [this](std::shared_ptr<fakenet::NetworkLink> netlink,
            const transport::SocketAddress& addr,
            const std::string& name) {
            logger::info("Client %s endpoint uses address %s",
                "IntegrationTest",
                name.c_str(),
                addr.toString().c_str());
            if (netlink)
            {
                this->_clientNetworkLinkMap.emplace(name, NetworkLinkInfo{netlink.get(), addr});
            }
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
                if (netLink)
                {
                    this->_endpointNetworkLinkMap.emplace(name, NetworkLinkInfo{netLink.get(), addr});
                }
            }));

    _bridge->initialize(_bridgeEndpointFactory, *_httpd, _smbInterfaces);

    initLocalTransports(config);
}

void IntegrationTest::initLocalTransports(config::Config& bridgeConfig)
{
    _sslDtls = &_bridge->getSslDtls();
    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);

    std::string configJson =
        utils::format(R"({"ice.preferredIp": "%s", "ice.singlePort":10050, "recording.singlePort":0})",
            _ipv4.client.c_str());

    _clientConfig.readFromString(configJson);
    _clientConfig.ice.enableIpv6 = bridgeConfig.ice.enableIpv6.get();

    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_ipv4.client, 0));
    interfaces.push_back(transport::SocketAddress::parse(_ipv6.client, 0));

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

    std::vector<transport::SocketAddress> publicInterfaces;
    publicInterfaces.push_back(transport::SocketAddress::parse("205.55.61.10", 0));

    _publicTransportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _clientConfig,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControlConfig,
        publicInterfaces,
        *_network,
        *_mainPoolAllocator,
        _bridgeEndpointFactory);

    for (const auto& linkInfo : _endpointNetworkLinkMap)
    {
        // SFU's default downlinks is good (1 Gbps).
        linkInfo.second.ptrLink->setBandwidthKbps(1000000);
    }
}

using namespace emulator;

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

    logger::debug("conference endpoint json %s", "IntegrationTest", endpointRequestBody.dump().c_str());
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

void IntegrationTest::runTestInThread(const size_t expectedNumThreads,
    std::function<void()> test,
    uint32_t maxTestDurationSec)
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
        _timeSource.runFor(maxTestDurationSec * utils::Time::sec);

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

void IntegrationTest::enterRealTime(size_t expectedThreadCount, const uint64_t timeout)
{
    _timeSource.waitForThreadsToSleep(expectedThreadCount, timeout);
    utils::Time::initialize();

    // release all sleeping threads into real time to finish the test
    _timeSource.shutdown();
}
