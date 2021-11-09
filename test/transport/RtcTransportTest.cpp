#include "SrtpUnprotectJob.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcPublish.h"
#include "config/Config.h"
#include "jobmanager/WorkerThread.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/macros.h"
#include "test/transport/SendJob.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "transport/sctp/SctpConfig.h"
#include "utils/Pacer.h"
#include "utils/Time.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;
using namespace jobmanager;
using namespace concurrency;
using namespace transport;
using namespace testing;

struct RtcTransportTest : public testing::TestWithParam<std::tuple<uint32_t, bool>>
{
    std::unique_ptr<config::Config> _config;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    utils::Pacer _pacer;
    std::unique_ptr<TransportFactory> _transportFactory;

    RtcTransportTest()
        : _config(std::make_unique<config::Config>()),
          _jobManager(std::make_unique<jobmanager::JobManager>()),
          _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(4096 * 32, "testMain")),
          _sslDtls(std::make_unique<transport::SslDtls>()),
          _srtpClientFactory(std::make_unique<transport::SrtpClientFactory>(*_sslDtls, *_mainPoolAllocator)),
          _network(transport::createRtcePoll()),
          _pacer(5 * 1000000)
    {

        for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
        {
            _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
        }
    }

    void SetUp() override
    {
        utils::Time::initialize();
        _bweConfig.sanitize();

        std::string configJson = "{\"rtc.ip\": \"127.0.0.1\", \"ice.singlePort\": 10008}";
        std::vector<transport::SocketAddress> interfaces;
        interfaces.push_back(transport::SocketAddress::parse("127.0.0.1", 0));
        _config->readFromString(configJson);
        _transportFactory = transport::createTransportFactory(*_jobManager,
            *_srtpClientFactory,
            *_config,
            _sctpConfig,
            _iceConfig,
            _bweConfig,
            _rateControlConfig,
            interfaces,
            *_network,
            *_mainPoolAllocator);
    }

    void TearDown() override
    {
        _transportFactory.reset();
        _network->stop();
        _jobManager->stop();
        for (auto& wt : _workerThreads)
        {
            wt->stop();
        }
    }
};

struct ClientPair : public transport::DataReceiver, public transport::DecryptedPacketReceiver
{
    struct ReportTrack
    {
        uint64_t previousReport = utils::Time::getAbsoluteTime();
        uint64_t previousLog = utils::Time::getAbsoluteTime();
        uint64_t receivedByteCount = 0;
        uint32_t lossCount = 0;
    };

    struct FakeMediaSources
    {
        FakeMediaSources(memory::PacketPoolAllocator& allocator, uint32_t ssrc)
            : audio(allocator, 90, ssrc),
              video(allocator, 4200, ssrc + 1)
        {
        }

        fakenet::FakeAudioSource audio;
        fakenet::FakeVideoSource video;
    };

    ClientPair(TransportFactory* transportFactory,
        uint32_t ssrc,
        size_t allocatorPacketCount,
        transport::SslDtls& sslDtls,
        JobManager& jobManager,
        bool enableIce)
        : _name("ClientPair"),
          _ssrc(ssrc),
          _sendAllocator(allocatorPacketCount, _name.c_str()),
          _transport1(enableIce ? transportFactory->createOnSharedPort(ice::IceRole::CONTROLLED, 128, 1)
                                : transportFactory->create(128, 1)),
          _transport2(enableIce ? transportFactory->createOnPrivatePort(ice::IceRole::CONTROLLING, 128, 2)
                                : transportFactory->create(128, 2)),
          _jobsCounter1(1),
          _jobsCounter2(1),
          _jobManager(jobManager),
          _media1(_sendAllocator, ssrc),
          _media2(_sendAllocator, ssrc + 2)
    {
        _report.emplace(_transport1.get(), ReportTrack());
        _report.emplace(_transport2.get(), ReportTrack());
        while (!_transport1->isGatheringComplete() || !_transport2->isGatheringComplete())
        {
            usleep(100000);
        }
        _transport1->setDataReceiver(this);
        _transport2->setDataReceiver(this);

        _transport1->start();
        _transport2->start();

        if (enableIce)
        {
            _transport1->setRemoteIce(_transport2->getLocalCredentials(),
                _transport2->getLocalCandidates(),
                _sendAllocator);
            _transport2->setRemoteIce(_transport1->getLocalCredentials(),
                _transport1->getLocalCandidates(),
                _sendAllocator);
        }
        else
        {
            _transport1->setRemotePeer(_transport2->getLocalRtpPort());
            _transport2->setRemotePeer(_transport1->getLocalRtpPort());
        }

        _transport1->setRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), true);
        _transport2->setRemoteDtlsFingerprint("sha-256", sslDtls.getLocalFingerprint(), false);
        _transport1->setAbsSendTimeExtensionId(3); // from FakeVideoSource
        _transport2->setAbsSendTimeExtensionId(3);
        _transport1->setAudioPayloadType(_media2.audio.getPayloadType(), 16000);
        _transport2->setAudioPayloadType(_media1.audio.getPayloadType(), 16000);
        _transport1->connect();
        _transport2->connect();
    }

    void stop()
    {
        if (_transport1)
        {
            _transport1->stop();
        }
        if (_transport2)
        {
            _transport2->stop();
        }
    }

    ~ClientPair()
    {
        --_jobsCounter1;
        --_jobsCounter2;
        auto start = utils::Time::getAbsoluteTime();

        while (_jobsCounter1 > 0 || _jobsCounter2 > 0 || _transport1->hasPendingJobs() || _transport2->hasPendingJobs())
        {
            std::this_thread::yield();
            if (utils::Time::diffGT(start, utils::Time::getAbsoluteTime(), utils::Time::ms * 250))
            {
                logger::error("job counts not released %u, %u",
                    _name.c_str(),
                    _jobsCounter1.load(),
                    _jobsCounter2.load());
                break;
            }
        }
    }

    void processTick(uint64_t timestamp)
    {
        forwardMedia(timestamp, _media2.audio, _transport2.get());
        forwardMedia(timestamp, _media2.video, _transport2.get());
    }

    int64_t timeToNextTick(uint64_t timestamp)
    {
        return std::min(_media1.audio.timeToRelease(timestamp),
            std::min(_media1.video.timeToRelease(timestamp),
                std::min(_media2.audio.timeToRelease(timestamp), _media2.video.timeToRelease(timestamp))));
    }

    void forwardMedia(uint64_t timestamp, fakenet::MediaSource& src, transport::Transport* transport)
    {
        for (auto packet = src.getPacket(timestamp); packet; packet = src.getPacket(timestamp))
        {
            transport->getJobQueue().addJob<transport::SendJob>(*transport, packet, _sendAllocator);
        }
    }

    bool isConnected() { return _transport1->isConnected() && _transport2->isConnected(); }

    void onRtpPacketReceived(RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        const uint32_t extendedSequenceNumber,
        uint64_t timestamp) override
    {
        auto& report = _report[sender];
        if (utils::Time::diffGT(report.previousReport, timestamp, utils::Time::sec * 2))
        {
            auto audioCounters = sender->getAudioReceiveCounters(timestamp);
            auto videoCounters = sender->getVideoReceiveCounters(timestamp);
            report.receivedByteCount = (audioCounters + videoCounters).octets;
            logger::debug("report %" PRIu64, "", report.receivedByteCount);
            if ((audioCounters + videoCounters).lostPackets > 0)
            {
                report.lossCount += (audioCounters + videoCounters).lostPackets;
                logger::warn("channel %u rate %ukbps, loss %.2f",
                    sender->getLoggableId().c_str(),
                    _ssrc,
                    (audioCounters + videoCounters).bitrateKbps,
                    (audioCounters + videoCounters).getReceiveLossRatio());
            }

            if (utils::Time::diffGT(report.previousLog, timestamp, utils::Time::sec * 5))
            {
                auto rate =
                    (audioCounters + videoCounters).octets * 8 * utils::Time::ms / (timestamp - report.previousReport);
                if (rate < _media1.video.getBandwidth())
                {
                    logger::debug("inbound: %" PRIu64 "kbps", sender->getLoggableId().c_str(), rate);
                }
                report.previousLog = timestamp;
            }
            report.previousReport = timestamp;
        }

        sender->getJobQueue().addJob<transport::SrtpUnprotectJob>(sender, packet, receiveAllocator, this);
    }

    void onRtcpPacketDecoded(transport::RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        const uint64_t timestamp) override
    {
        receiveAllocator.free(packet);
    }

    void onRtpPacketDecrypted(transport::RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        std::atomic_uint32_t& ownerCount) override
    {
        receiveAllocator.free(packet);
    }

    void onConnected(RtcTransport*) override {}
    bool onSctpConnectionRequest(RtcTransport*, uint16_t remotePort) override { return true; }
    void onSctpEstablished(RtcTransport* sender) override{};
    void onSctpMessage(RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override{};

    void onRecControlReceived(RecordingTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint64_t timestamp) override{};

    logger::LoggableId _name;
    uint32_t _ssrc;
    memory::PacketPoolAllocator _sendAllocator;
    std::shared_ptr<RtcTransport> _transport1;
    std::shared_ptr<RtcTransport> _transport2;
    std::atomic_uint32_t _jobsCounter1;
    std::atomic_uint32_t _jobsCounter2;

    jobmanager::JobManager& _jobManager;

    FakeMediaSources _media1;
    FakeMediaSources _media2;

    std::unordered_map<Transport*, ReportTrack> _report;
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

TEST_P(RtcTransportTest, packetLoad)
{
    const int CLIENT_COUNT = std::get<0>(GetParam());
    const bool enableIce = std::get<1>(GetParam());
    std::vector<std::unique_ptr<ClientPair>> testPairs;

    const auto TEST_DURATION = utils::Time::sec * 15;
    logger::info("running test with %d client channels", "packetLoadTest", CLIENT_COUNT);

    for (int i = 0; i < CLIENT_COUNT; ++i)
    {
        testPairs.emplace_back(std::make_unique<ClientPair>(_transportFactory.get(),
            1000 + i * 4,
            2048,
            *_sslDtls,
            *_jobManager,
            enableIce));
    }

    while (!areAllConnected(testPairs))
    {
        utils::Time::nanoSleep(utils::Time::sec * 1);
        logger::debug("Not connected all yet", "");
    }

    const uint64_t testStart = utils::Time::getAbsoluteTime();
    int laggingCount = 0;
    for (;;)
    {
        const uint64_t tickStart = utils::Time::getAbsoluteTime();
        if (utils::Time::diffGE(testStart, tickStart, TEST_DURATION))
        {
            break;
        }

        for (int i = 0; i < CLIENT_COUNT; ++i)
        {
            testPairs[i]->processTick(tickStart);
        }

        int64_t toSleep = utils::Time::ms * 20;
        const auto timestamp = utils::Time::getAbsoluteTime();
        for (int i = 0; i < CLIENT_COUNT; ++i)
        {
            toSleep = std::min(toSleep, testPairs[i]->timeToNextTick(timestamp));
            if (toSleep == 0)
            {
                break;
            }
        }

        if (utils::Time::getAbsoluteTime() - tickStart > 20 * utils::Time::ms)
        {
            logger::info("tick took %" PRIu64 "ms",
                "TransportTest",
                (utils::Time::getAbsoluteTime() - tickStart) / utils::Time::ms);
        }

        if (toSleep > 0)
        {
            laggingCount = 0;
            utils::Time::nanoSleep(toSleep);
        }
        else if (++laggingCount > 1250)
        {
            logger::error("aborting test. Cannot keep up", "");
            break;
        }
    }

    for (auto& clientpair : testPairs)
    {
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(clientpair->_report[clientpair->_transport1.get()].receivedByteCount, 85000);
            EXPECT_EQ(clientpair->_report[clientpair->_transport1.get()].lossCount, 0);
        }
        clientpair->stop();
    }

    testPairs.clear();
}

INSTANTIATE_TEST_SUITE_P(DISABLED_Performance,
    RtcTransportTest,
    testing::Combine(testing::Values(10, 100, 200, 300, 400, 500, 750, 800, 1200, 1500), testing::Values(true)));

INSTANTIATE_TEST_SUITE_P(RecPerformance,
    RtcTransportTest,
    testing::Combine(testing::Values(5, 10), testing::Values(false)));

TEST(TransportStats, packetLoss)
{
    config::Config config;
    transport::RtpReceiveState state(config);

    auto timestamp = utils::Time::getAbsoluteTime();
    for (int i = 0; i < 200; ++i)
    {
        memory::Packet packet;
        packet.setLength(200);
        auto header = rtp::RtpHeader::create(packet.get(), packet.getLength());

        header->ssrc = 101;
        header->sequenceNumber = i;
        header->timestamp = 6700 + i * 160;
        timestamp += 20 * utils::Time::ms;

        if (i % 10 != 1)
        {
            state.onRtpReceived(packet, timestamp);
        }
    }

    transport::PacketCounters snapshot = state.getCounters();
    EXPECT_EQ(snapshot.lostPackets, 5);
    EXPECT_EQ(snapshot.getReceiveLossRatio(), 0.1);
}

TEST(TransportStats, outboundLoss)
{
    config::Config config;
    transport::RtpSenderState state(16000, config);

    memory::Packet packet;

    auto report = rtp::RtcpReceiverReport::create(packet.get());

    auto timestamp = utils::Time::getAbsoluteTime();
    auto wallClock = std::chrono::system_clock::now();
    report->ssrc = 101;

    auto& block = report->addReportBlock(6000);
    block.extendedSeqNoReceived = 39;
    block.delaySinceLastSR = 12312312;
    block.loss.setCumulativeLoss(20);
    block.loss.setFractionLost(0.12);

    for (int i = 0; i < 100; ++i)
    {
        memory::Packet rtpPacket;
        auto header = rtp::RtpHeader::create(rtpPacket.get(), memory::Packet::size);
        header->ssrc = 6000;
        header->sequenceNumber = i;
        header->payloadType = 8;
        rtpPacket.setLength(130);
        state.onRtpSent(timestamp, rtpPacket);
        timestamp += 1000;
    }
    timestamp += utils::Time::ms * 50;
    wallClock += std::chrono::milliseconds(50);
    state.onReceiverBlockReceived(timestamp, utils::Time::toNtp32(wallClock), report->reportBlocks[0]);

    transport::PacketCounters snapshot = state.getCounters();
    EXPECT_EQ(snapshot.lostPackets, 20);
    EXPECT_EQ(snapshot.getSendLossRatio(), 0.50);
}

TEST(TransportStats, MpmcPublish)
{
    struct InfoObject
    {
        uint32_t mem[20];
        uint32_t id = 0;
    };

    const int THREAD_COUNT = 6;
    const int WRITER_COUNT = 2;
    concurrency::MpmcPublish<InfoObject, THREAD_COUNT + WRITER_COUNT> board;

    board.write(InfoObject());
    std::atomic_bool running(true);
    std::thread* threads[THREAD_COUNT];
    std::thread* writers[WRITER_COUNT];

    for (int i = 0; i < WRITER_COUNT; ++i)
    {
        writers[i] = new std::thread([&board, &running]() {
            InfoObject data;
            while (running)
            {
                EXPECT_TRUE(board.write(data));
                utils::Time::usleep(10);
            }
        });
    }

    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        threads[i] = new std::thread([&board, &running]() {
            InfoObject data;
            while (running)
            {
                EXPECT_TRUE(board.read(data));
            }
        });
    }

    utils::Time::usleep(5000000);
    running = false;
    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        threads[i]->join();
        delete threads[i];
    }
    for (int i = 0; i < WRITER_COUNT; ++i)
    {
        writers[i]->join();
        delete writers[i];
    }
}

TEST(TransportStats, MpmcPublish2)
{
    struct InfoObject
    {
        uint32_t mem[20];
        uint32_t id = 0;
    };

    MpmcPublish<int, 8> data;

    for (int i = 0; i < 1000; ++i)
    {
        data.write(i);
        int r = 9000;
        EXPECT_TRUE(data.read(r));
        EXPECT_EQ(i, r);
    }
}
