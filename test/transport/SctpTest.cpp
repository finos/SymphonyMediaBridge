#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "config/Config.h"
#include "jobmanager/WorkerThread.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "test/transport/SendJob.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "transport/sctp/SctpConfig.h"
#include "utils/Pacer.h"
#include "utils/Time.h"
#include "webrtc/DataChannel.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;
using namespace jobmanager;
using namespace concurrency;
using namespace transport;
using namespace testing;
/*using ::testing::NiceMock;
using ::testing::Matcher;
using ::testing::_;
*/
namespace
{
const char* theMessage = "My message is important";

struct ClientPair : public transport::DataReceiver
{

    class SendJob : public jobmanager::CountedJob
    {
    public:
        SendJob(Transport& transport, memory::UniquePacket packet)
            : CountedJob(transport.getJobCounter()),
              _transport(transport),
              _packet(std::move(packet))
        {
        }

        void run() override { _transport.protectAndSend(std::move(_packet)); }

    private:
        Transport& _transport;
        memory::UniquePacket _packet;
    };

    ClientPair(TransportFactory* transportFactory,
        uint32_t ssrc,
        memory::PacketPoolAllocator& allocator,
        transport::SslDtls& sslDtls,
        JobManager& jobManager)
        : _ssrc(ssrc),
          _sendAllocator(allocator),
          _transport1(transportFactory->create(1)),
          _transport2(transportFactory->create(2)),
          _sequenceNumber(0),
          _tickCount(0),
          _receivedByteCount(0),
          _receivedPacketCount(0),
          _jobManager(jobManager),
          _streamId(0)
    {
        while (!_transport1->isGatheringComplete() || !_transport2->isGatheringComplete())
        {
            usleep(100000);
        }

        _transport1->start();
        _transport2->start();
        _transport1->setDataReceiver(this);
        _transport2->setDataReceiver(this);

        _transport1->setRemotePeer(_transport2->getLocalRtpPort());
        _transport2->setRemotePeer(_transport1->getLocalRtpPort());

        _transport1->asyncSetRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), true);
        _transport2->asyncSetRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), false);

        _transport1->setSctp(5000, 5001);
        _transport2->setSctp(5001, 5000);
        _transport1->connect();
        _transport2->connect();
    }

    ~ClientPair()
    {
        if (_transport1)
        {
            _transport1->stop();
        }
        if (_transport2)
        {
            _transport2->stop();
        }

        while (_transport2->hasPendingJobs() || _transport1->hasPendingJobs())
        {
            std::this_thread::yield();
        }
    }

    void processTick()
    {
        if (_tickCount++ % 2)
        {
            return;
        }

        if (_sctpEstablished && (_tickCount % 25) == 0)
        {
            ++_messagesSent;
            _messageBytesSent += std::strlen(theMessage);
            _transport1->sendSctp(_streamId,
                webrtc::DataChannelPpid::WEBRTC_STRING,
                theMessage,
                std::strlen(theMessage));
        }

        for (int j = 0; j < 1; ++j)
        {
            auto packet = memory::makeUniquePacket(_sendAllocator);
            packet->setLength(160 + rtp::MIN_RTP_HEADER_SIZE);

            auto rtpHeader = rtp::RtpHeader::create(*packet);
            rtpHeader->payloadType = 8;
            rtpHeader->ssrc = _ssrc;
            rtpHeader->sequenceNumber = _sequenceNumber++;

            _transport1->getJobQueue().addJob<transport::SendJob>(*_transport1, std::move(packet));
        }
    }

    bool isConnected() { return _transport1->isConnected() && _transport2->isConnected(); }

    void onRtpPacketReceived(RtcTransport* sender,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber,
        uint64_t timestamp) override
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

    void onRtcpPacketDecoded(transport::RtcTransport* sender,
        memory::UniquePacket packet,
        const uint64_t timestamp) override
    {
    }

    void onConnected(RtcTransport* transport) override
    {
        logger::info("transport connected ", transport->getLoggableId().c_str());
    }
    bool onSctpConnectionRequest(RtcTransport*, uint16_t remotePort) override { return true; }
    void onSctpEstablished(RtcTransport* sender) override
    {
        _sctpEstablished = true;
        _streamId = sender->allocateOutboundSctpStream();
        logger::info("SCTP established ", sender->getLoggableId().c_str());
    }

    void onSctpMessage(RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override
    {
        auto dataPtr = reinterpret_cast<const char*>(data);
        if (sender == _transport1.get())
        {
            _recv1.append(dataPtr, length);
        }
        else
        {
            _recv2.append(dataPtr, length);
        }
    };

    void onRecControlReceived(RecordingTransport* sender, memory::UniquePacket packet, uint64_t timestamp) override {}

    void onIceReceived(transport::RtcTransport* sender, uint64_t timestamp) override {}

    uint32_t _ssrc;
    memory::PacketPoolAllocator& _sendAllocator;
    std::shared_ptr<RtcTransport> _transport1;
    std::shared_ptr<RtcTransport> _transport2;
    uint16_t _sequenceNumber;
    uint32_t _tickCount;

    std::atomic_uint32_t _receivedByteCount;
    time_t _receiveStart;
    std::atomic_uint32_t _receivedPacketCount;

    jobmanager::JobManager& _jobManager;

    std::atomic<bool> _sctpEstablished = {false};
    uint16_t _streamId;
    std::string _recv1;
    std::string _recv2;
    int _messagesSent = 0;
    int _messageBytesSent = 0;
};

struct SctpTransportTest : public ::testing::Test
{
    std::unique_ptr<TransportFactory> _transportFactory;

    memory::PacketPoolAllocator _sendAllocator;
    std::unique_ptr<config::Config> _config;
    std::unique_ptr<jobmanager::TimerQueue> _timers;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainAllocator;
    std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    sctp::SctpConfig _sctpConfig;
    utils::Pacer _pacer;
    std::vector<std::unique_ptr<ClientPair>> _testPairs;

    SctpTransportTest()
        : _sendAllocator(memory::packetPoolSize, "TransportTest"),
          _config(nullptr),
          _timers(std::make_unique<jobmanager::TimerQueue>(4096 * 8)),
          _jobManager(std::make_unique<jobmanager::JobManager>(*_timers)),
          _mainAllocator(std::make_unique<memory::PacketPoolAllocator>(1024, "SctpTest")),
          _sslDtls(nullptr),
          _srtpClientFactory(nullptr),
          _network(transport::createRtcePoll()),
          _pacer(10 * 1000000)
    {
        for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
        {
            _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager, true));
        }
    }

    void SetUp() override
    {
        _config = std::make_unique<config::Config>();
        _sslDtls = std::make_unique<transport::SslDtls>();
        _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);

        std::string configJson = "{\"rtc.ip\": \"127.0.0.1\"}";
        _config->readFromString(configJson);
        std::vector<transport::SocketAddress> interfaces;
        interfaces.push_back(transport::SocketAddress::parse("127.0.0.1", 0));

        _transportFactory = transport::createTransportFactory(*_jobManager,
            *_srtpClientFactory,
            *_config,
            _sctpConfig,
            _iceConfig,
            _bweConfig,
            _rateControlConfig,
            interfaces,
            *_network,
            *_mainAllocator,
            std::shared_ptr<EndpointFactory>(new transport::EndpointFactoryImpl()));
    }

    void TearDown() override
    {
        _testPairs.clear();
        _transportFactory.reset();
        _network->stop();
        _timers->stop();
        _jobManager->stop();

        for (auto& wt : _workerThreads)
        {
            wt->stop();
        }
    }
};

bool areAllConnected(std::vector<std::unique_ptr<ClientPair>>& testPairs)
{
    for (size_t i = 0; i < testPairs.size(); ++i)
    {
        if (!testPairs[i]->isConnected())
        {
            return false;
        }
    }

    return true;
}

} // namespace

TEST_F(SctpTransportTest, messages)
{
    const int CLIENT_COUNT = 1;
    const int TEST_DURATION = 2000;
    const int TICK_DURATION = 10;

    for (int i = 0; i < CLIENT_COUNT; ++i)
    {
        _testPairs.emplace_back(
            std::make_unique<ClientPair>(_transportFactory.get(), 1000 + i, _sendAllocator, *_sslDtls, *_jobManager));
    }

    while (!areAllConnected(_testPairs))
    {
        usleep(100);
    }
    _pacer.reset(utils::Time::getAbsoluteTime());

    for (int i = 0; i < TEST_DURATION; i += TICK_DURATION)
    {
        const uint64_t start = utils::Time::getAbsoluteTime();
        _pacer.tick(start);
        for (int i = 0; i < CLIENT_COUNT; ++i)
        {
            _testPairs[i]->processTick();
        }

        int64_t toSleep = _pacer.timeToNextTick(utils::Time::getAbsoluteTime()) / 1000;
        if (toSleep > 0)
        {
            usleep(toSleep);
        }
    }

    for (auto& clientpair : _testPairs)
    {
        EXPECT_GT(clientpair->_receivedByteCount, 18000);
        EXPECT_EQ(clientpair->_recv1.size(), 0);
        EXPECT_EQ(clientpair->_recv2.size(), clientpair->_messageBytesSent);
        EXPECT_EQ(std::strcmp(theMessage, clientpair->_recv2.substr(0, strlen(theMessage)).c_str()), 0);
    }
}
