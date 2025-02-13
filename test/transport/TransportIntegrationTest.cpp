#include "TransportIntegrationTest.h"
#include "jobmanager/WorkerThread.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"

using namespace testing;

TransportIntegrationTest::TransportIntegrationTest()
    : _sendAllocator(memory::packetPoolSize, "TransportTest"),
      _audioAllocator(memory::packetPoolSize, "TransportTestAudio"),
      _timers(std::make_unique<jobmanager::TimerQueue>(4096 * 8)),
      _jobManager(std::make_unique<jobmanager::JobManager>(*_timers)),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(4096, "testMain")),
      _sslDtls(std::make_unique<transport::SslDtls>()),
      _srtpClientFactory(std::make_unique<transport::SrtpClientFactory>(*_sslDtls)),
      _network(transport::createRtcePoll()),
      _pacer(10 * 1000000)
{

    for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager, true));
    }
}

void TransportIntegrationTest::SetUp()
{
    std::string configJson1 = "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":0, \"recording.singlePort\":0}";
    std::string configJson2 =
        "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10010, \"recording.singlePort\":0}";
    _config1.readFromString(configJson1);
    _config2.readFromString(configJson2);
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_config1.ice.preferredIp, 0));

    init(configJson1, configJson2);
}

bool TransportIntegrationTest::init(std::string configJson1, std::string configJson2)
{
    _transportFactory1.reset();
    _transportFactory2.reset();

    _config1.readFromString(configJson1);
    _config2.readFromString(configJson2);
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_config1.ice.preferredIp, 0));

    _transportFactory1 = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _config1,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControlConfig,
        interfaces,
        *_network,
        *_mainPoolAllocator,
        std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl()));
    {
        std::vector<transport::SocketAddress> interfaces;
        interfaces.push_back(transport::SocketAddress::parse(_config2.ice.preferredIp, 0));
        _transportFactory2 = transport::createTransportFactory(*_jobManager,
            *_srtpClientFactory,
            _config2,
            _sctpConfig,
            _iceConfig,
            _bweConfig,
            _rateControlConfig,
            interfaces,
            *_network,
            *_mainPoolAllocator,
            std::shared_ptr<transport::EndpointFactory>(new transport::EndpointFactoryImpl()));
    }

    return _transportFactory1->isGood() && _transportFactory2->isGood();
}

void TransportIntegrationTest::TearDown()
{
    _transportFactory1.reset();
    _transportFactory2.reset();
    _network->stop();
    _timers->stop();
    _jobManager->stop();
    for (auto& wt : _workerThreads)
    {
        wt->stop();
    }
}

TransportClientPair::TransportClientPair(transport::TransportFactory& transportFactory1,
    transport::TransportFactory& transportFactory2,
    uint32_t ssrc,
    memory::PacketPoolAllocator& allocator,
    memory::AudioPacketPoolAllocator& audioAllocator,
    transport::SslDtls& sslDtls,
    jobmanager::JobManager& jobManager,
    bool blockUdp)
    : _ssrc(ssrc),
      _sendAllocator(allocator),
      _audioAllocator(audioAllocator),
      _transport1(transportFactory1.create(ice::IceRole::CONTROLLING, 1)),
      _transport2(transportFactory2.create(ice::IceRole::CONTROLLED, 2)),
      _sequenceNumber(0),
      _tickCount(0),
      _connectStart(0),
      _signalDelay(0),
      _receivedByteCount(0),
      _receivedPacketCount(0),
      _jobManager(jobManager)
{
    _transport1->start();
    _transport2->start();
    _transport1->setDataReceiver(this);
    _transport2->setDataReceiver(this);

    _candidates1 = _transport1->getLocalCandidates();
    auto candidates2 = _transport2->getLocalCandidates();

    if (blockUdp)
    {
        for (auto& candidate : _candidates1)
        {
            if (candidate.transportType == ice::TransportType::UDP)
            {
                candidate.address.setPort(candidate.address.getPort() + 1);
            }
        }
        for (auto& candidate : candidates2)
        {
            if (candidate.transportType == ice::TransportType::UDP)
            {
                candidate.address.setPort(candidate.address.getPort() + 1);
            }
        }
    }

    _transport1->setRemoteIce(_transport2->getLocalIceCredentials(), candidates2, _audioAllocator);
    _transport1->asyncSetRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), true);
}

void TransportClientPair::stop()
{
    _transport1->setDataReceiver(nullptr);
    _transport2->setDataReceiver(nullptr);
    if (_transport1)
    {
        _transport1->stop();
    }
    if (_transport2)
    {
        _transport2->stop();
    }

    while ((_transport1 && _transport1->hasPendingJobs()) || (_transport2 && _transport2->hasPendingJobs()))
    {
        std::this_thread::yield();
    }
}

TransportClientPair::~TransportClientPair()
{
    EXPECT_TRUE(!(_transport1 && _transport1->hasPendingJobs()));
    EXPECT_TRUE(!(_transport2 && _transport2->hasPendingJobs()));
}

void TransportClientPair::connect(uint64_t timestamp)
{
    _transport1->connect();
    _connectStart = timestamp;
    _signalDelay = utils::Time::us * (rand() % 125000);
}

int64_t TransportClientPair::tryConnect(const uint64_t timestamp, const transport::SslDtls& sslDtls)
{
    if (_connectStart == 0)
    {
        return utils::Time::sec;
    }

    if (utils::Time::diffLT(_connectStart, timestamp, _signalDelay))
    {
        return utils::Time::diff(_connectStart + _signalDelay, timestamp);
    }

    _transport2->setRemoteIce(_transport1->getLocalIceCredentials(), _candidates1, _audioAllocator);
    _transport2->asyncSetRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), false);
    _transport2->connect();
    _connectStart = 0;
    return utils::Time::sec;
}

int64_t TransportClientPair::tryConnect(const uint64_t timestamp,
    const transport::SslDtls& sslDtls,
    std::pair<std::string, std::string> transport1IceCredentials)
{
    if (_connectStart == 0)
    {
        return utils::Time::sec;
    }

    if (utils::Time::diffLT(_connectStart, timestamp, _signalDelay))
    {
        return utils::Time::diff(_connectStart + _signalDelay, timestamp);
    }

    _transport2->setRemoteIce(transport1IceCredentials, _candidates1, _audioAllocator);
    _transport2->asyncSetRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), false);
    _transport2->connect();
    _connectStart = 0;
    return utils::Time::sec;
}

bool TransportClientPair::isConnected()
{
    return _transport1->isConnected() && _transport2->isConnected();
}

void TransportClientPair::onRtpPacketReceived(transport::RtcTransport* sender,
    memory::UniquePacket packet,
    const uint32_t extendedSequenceNumber,
    uint64_t timestamp)
{
    if (_receivedByteCount == 0)
        _receiveStart = std::time(nullptr);

    _receivedByteCount += packet->getLength();

    if ((_receivedPacketCount++ % 250) == 0)
    {
        logger::info("channel %u bit rate %ld, rcv %u, packets %u",
            "ClientPair",
            _ssrc,
            _receivedByteCount.load() * 8u / (1u + std::time(nullptr) - _receiveStart),
            _receivedByteCount.load(),
            _receivedPacketCount.load());
    }
}
