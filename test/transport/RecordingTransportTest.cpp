#include "transport/RecordingTransport.h"
#include "config/Config.h"
#include "jobmanager/WorkerThread.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "test/transport/EndpointListenerMock.h"
#include "test/transport/SendJob.h"
#include "transport/RtcePoll.h"
#include "transport/UdpEndpoint.h"
#include "transport/recp/RecStreamAddedEventBuilder.h"
#include "utils/Time.h"
#include <cinttypes>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

using namespace std;
using namespace jobmanager;
using namespace concurrency;
using namespace transport;
using namespace testing;

struct RecordingTransportTest : public testing::Test
{
    unique_ptr<config::Config> _config;
    unique_ptr<jobmanager::JobManager> _jobManager;
    unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    unique_ptr<RtcePoll> _rtcePoll;
    vector<unique_ptr<WorkerThread>> _workerThreads;

    RecordingTransportTest()
        : _config(make_unique<config::Config>()),
          _jobManager(make_unique<JobManager>()),
          _mainPoolAllocator(make_unique<memory::PacketPoolAllocator>(4096 * 32, "testMain")),
          _rtcePoll(createRtcePoll())
    {
        for (size_t i = 0; i < thread::hardware_concurrency(); ++i)
        {
            _workerThreads.push_back(make_unique<WorkerThread>(*_jobManager));
        }
    }

    void SetUp() override
    {
        utils::Time::initialize();
        _config->readFromString(R"({"rtc.ip": "127.0.0.1", "ice.singlePort": 100010})");
    }

    void TearDown() override
    {
        _jobManager->stop();
        _rtcePoll->stop();
        for (auto& wt : _workerThreads)
        {
            wt->stop();
        }
    }
};

struct PacketGenerator
{
    memory::PacketPoolAllocator& allocator;
    uint16_t rtpSsrc;
    uint32_t rtpTimestamp;
    uint8_t rtpPayload;
    uint32_t counter;

    explicit PacketGenerator(memory::PacketPoolAllocator& packetAllocator)
        : allocator(packetAllocator),
          rtpSsrc(1),
          rtpTimestamp(3000),
          rtpPayload(100),
          counter(0)
    {
    }

    memory::PacketPtr generate() { return createFakePacket(); }

    memory::PacketPtr generateStreamAddEvent() { return createStreamAddEvent(); }

private:
    memory::PacketPtr createFakePacket()
    {
        auto packet = memory::makePacketPtr(allocator);
        if (packet)
        {
            auto rtpHeader = rtp::RtpHeader::create(*packet);
            rtpHeader->payloadType = rtpPayload;
            rtpHeader->sequenceNumber = counter++;
            rtpHeader->ssrc = rtpSsrc;
            rtpHeader->timestamp = rtpTimestamp;
            rtpTimestamp += 16000;

            rtp::RtpHeaderExtension extensionHead(rtpHeader->getExtensionHeader());
            rtp::GeneralExtension1Byteheader absSendTime(3, 3);
            auto cursor = extensionHead.extensions().begin();
            extensionHead.addExtension(cursor, absSendTime);
            rtpHeader->setExtensions(extensionHead);

            packet->setLength(randomPacketSize());
            return packet;
        }
        return memory::PacketPtr();
    }

    memory::PacketPtr createStreamAddEvent()
    {
        return recp::RecStreamAddedEventBuilder(allocator)
            .setSequenceNumber(1)
            .setTimestamp(rtpTimestamp)
            .setSsrc(rtpSsrc)
            .setIsScreenSharing(false)
            .setRtpPayloadType(rtpPayload)
            .setBridgeCodecNumber(rtpPayload)
            .setEndpoint("test")
            .setWallClock(std::chrono::system_clock::now())
            .build();
    }

    static uint16_t randomPacketSize()
    {
        auto min = 1000;
        auto max = 1455;
        return min + rand() % (max + 1 - min); // NOLINT(cert-msc50-cpp)
    }
};

TEST_F(RecordingTransportTest, protectAndSend)
{
    uint8_t salt[12];
    for (uint8_t i = 0; i < 12; ++i)
    {
        salt[i] = i;
    }
    uint8_t key[32];
    for (uint8_t i = 0; i < 32; ++i)
    {
        key[i] = i;
    }

    auto sourceAddress = SocketAddress::parse("127.0.0.1", 20101);
    auto targetAddress = SocketAddress::parse("127.0.0.1", 20102);
    StrictMock<fakenet::EndpointListenerMock> listener;

    ON_CALL(listener, onRtpReceived)
        .WillByDefault(
            [](Endpoint& endpoint, const SocketAddress& source, const SocketAddress& target, memory::PacketPtr packet) {
            });
    EXPECT_CALL(listener, onRtpReceived(_, _, _, _)).Times(100);

    UdpEndpoint udpEndpoint(*_jobManager, 64, *_mainPoolAllocator, targetAddress, *_rtcePoll, true);
    udpEndpoint.registerListener(sourceAddress, &listener);
    udpEndpoint.start();

    RecordingEndpoint recordingEndpoint(*_jobManager, 64, *_mainPoolAllocator, sourceAddress, *_rtcePoll, true);
    recordingEndpoint.start();

    auto transport = make_unique<RecordingTransport>(*_jobManager,
        *_config,
        &recordingEndpoint,
        hash<string>{}("test"),
        hash<string>{}("sTest"),
        targetAddress,
        key,
        salt,
        *_mainPoolAllocator);
    transport->start();

    memory::PacketPoolAllocator senderAllocator(4096, string("TestSenderAllocator"));
    PacketGenerator packetGenerator(senderAllocator);
    for (auto i = 0; i < 100; ++i)
    {
        auto packet = packetGenerator.generate();
        if (packet)
        {
            transport->getJobQueue().addJob<transport::SendJob>(*transport, std::move(packet));
            this_thread::sleep_for(chrono::milliseconds(5));
        }
    }

    transport->stop();

    // Wait for pending jobs
    for (auto i = 0; i < 10 && transport->hasPendingJobs(); ++i)
    {
        this_thread::sleep_for(chrono::seconds(1));
    }

    if (transport->hasPendingJobs())
    {
        FAIL() << "Transport has not finished the pending jobs after 10 seconds";
    }

    const auto waitForClose = [](auto& endpoint) {
        for (auto i = 0; i < 10 && endpoint.getState() != Endpoint::State::CLOSED; ++i)
        {
            this_thread::sleep_for(chrono::seconds(1));
        }

        if (endpoint.getState() != Endpoint::State::CLOSED)
        {
            FAIL() << "Endpoint has not closed after 10 seconds";
        }
    };

    udpEndpoint.closePort();
    recordingEndpoint.closePort();

    waitForClose(udpEndpoint);
    waitForClose(recordingEndpoint);
}

TEST_F(RecordingTransportTest, protectAndSendTriggerRtcpSending)
{
    uint8_t salt[12];
    for (uint8_t i = 0; i < 12; ++i)
    {
        salt[i] = i;
    }
    uint8_t key[32];
    for (uint8_t i = 0; i < 32; ++i)
    {
        key[i] = i;
    }

    auto sourceAddress = SocketAddress::parse("127.0.0.1", 20101);
    auto targetAddress = SocketAddress::parse("127.0.0.1", 20102);
    StrictMock<fakenet::EndpointListenerMock> listener;

    ON_CALL(listener, onRtpReceived)
        .WillByDefault(
            [](Endpoint& endpoint, const SocketAddress& source, const SocketAddress& target, memory::PacketPtr packet) {
            });

    ON_CALL(listener, onRtcpReceived)
        .WillByDefault(
            [](Endpoint& endpoint, const SocketAddress& source, const SocketAddress& target, memory::PacketPtr packet) {
            });

    EXPECT_CALL(listener, onRtpReceived(_, _, _, _)).Times(100);
    EXPECT_CALL(listener, onRtcpReceived(_, _, _, _)).Times(AtLeast(1));

    UdpEndpoint udpEndpoint(*_jobManager, 64, *_mainPoolAllocator, targetAddress, *_rtcePoll, true);
    udpEndpoint.registerListener(sourceAddress, &listener);
    udpEndpoint.start();

    RecordingEndpoint recordingEndpoint(*_jobManager, 64, *_mainPoolAllocator, sourceAddress, *_rtcePoll, true);
    recordingEndpoint.start();

    auto transport = make_unique<RecordingTransport>(*_jobManager,
        *_config,
        &recordingEndpoint,
        hash<string>{}("test"),
        hash<string>{}("sTest"),
        targetAddress,
        key,
        salt,
        *_mainPoolAllocator);
    transport->start();

    memory::PacketPoolAllocator senderAllocator(4096, string("TestSenderAllocator"));
    PacketGenerator packetGenerator(senderAllocator);

    // To receive RTCP packets we have to first send StreamAdd event to make the transport aware of this ssrc
    transport->getJobQueue().addJob<transport::SendJob>(*transport, packetGenerator.generateStreamAddEvent());

    for (auto i = 0; i < 100; ++i)
    {
        auto packet = packetGenerator.generate();
        if (packet)
        {
            transport->getJobQueue().addJob<transport::SendJob>(*transport, std::move(packet));
            this_thread::sleep_for(chrono::milliseconds(5));
        }
    }

    transport->stop();

    // Wait for pending jobs
    for (auto i = 0; i < 10 && transport->hasPendingJobs(); ++i)
    {
        this_thread::sleep_for(chrono::seconds(1));
    }

    if (transport->hasPendingJobs())
    {
        FAIL() << "Transport has not finished the pending jobs after 10 seconds";
    }

    const auto waitForClose = [](auto& endpoint) {
        for (auto i = 0; i < 10 && endpoint.getState() != Endpoint::State::CLOSED; ++i)
        {
            this_thread::sleep_for(chrono::seconds(1));
        }

        if (endpoint.getState() != Endpoint::State::CLOSED)
        {
            FAIL() << "Endpoint has not closed after 10 seconds";
        }
    };

    udpEndpoint.closePort();
    recordingEndpoint.closePort();

    waitForClose(udpEndpoint);
    waitForClose(recordingEndpoint);
}
