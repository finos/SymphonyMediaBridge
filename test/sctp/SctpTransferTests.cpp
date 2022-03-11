
#include "SctpEndpoint.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/sctp/SctpAssociation.h"
#include "transport/sctp/SctpConfig.h"
#include "transport/sctp/SctpServerPort.h"
#include "utils/Time.h"
#include "webrtc/DataChannel.h"
#include "webrtc/DataStreamTransport.h"
#include "webrtc/WebRtcDataStream.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <memory>

using namespace std;
using namespace transport;
using namespace testing;
using namespace sctptest;

namespace
{

} // namespace

struct SctpTransferTestFixture : public ::testing::Test
{
    SctpTransferTestFixture() : _timestamp(utils::Time::getAbsoluteTime()) {}

    void establishConnection(SctpEndpoint& A, SctpEndpoint& B)
    {
        _timestamp += 10 * utils::Time::ms;
        A.connect(B._port->getPort());

        for (int i = 0; i < 100; ++i)
        {
            _timestamp += 2 * utils::Time::ms;
            A.process();
            B.process();
            A.forwardPackets(B);
            B.forwardPackets(A);
            if (!!B._session && A._session->getState() == sctp::SctpAssociation::State::ESTABLISHED &&
                B._session->getState() == sctp::SctpAssociation::State::ESTABLISHED)
            {
                return;
            }
        }
        FAIL();
    }

    void SetUp() override
    {
        logger::info("\nUnit TEST %s start", "gtest", ::testing::UnitTest::GetInstance()->current_test_info()->name());
    }
    void TearDown() override
    {
        utils::Time::nanoSleep(3 * utils::Time::ms); // let logs flush}
    }
    uint64_t _timestamp;
    sctp::SctpConfig _config;
};

TEST_F(SctpTransferTestFixture, send500K)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);

    establishConnection(A, B);

    const int DATA_SIZE = 450 * 1024;
    std::array<uint8_t, DATA_SIZE> data;
    std::memset(data.data(), 0xAA, DATA_SIZE);
    const auto startTime = _timestamp;
    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_BINARY,
        data.data(),
        DATA_SIZE,
        _timestamp);
    for (int i = 0; i < 280000 && B.getReceivedSize() < DATA_SIZE; ++i)
    {
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }

    utils::Time::nanoSleep(100 * utils::Time::ms);
    EXPECT_EQ(B.getReceivedSize(), DATA_SIZE);
    EXPECT_LE(_timestamp - startTime, 18 * utils::Time::sec);
}

TEST_F(SctpTransferTestFixture, sendMany)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);

    establishConnection(A, B);

    const int DATA_SIZE = 250 * 1024;
    std::array<uint8_t, 1450> data;
    std::memset(data.data(), 0xdd, data.size());
    const auto startTime = _timestamp;
    size_t totalSent = 0;
    size_t totalSentCount = 0;
    for (int i = 0; i < 300000 && B.getReceivedSize() < DATA_SIZE; ++i)
    {
        if (A._session->outboundPendingSize() < 16000)
        {
            auto toSend = std::max<int>(15, rand() % data.size());
            totalSent += toSend;
            ++totalSentCount;
            A._session->sendMessage(A.getStreamId(),
                webrtc::DataChannelPpid::WEBRTC_BINARY,
                data.data(),
                toSend,
                _timestamp);
        }
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }

    for (int i = 0; i < 20000; ++i)
    {
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }
    utils::Time::nanoSleep(100 * utils::Time::ms);
    EXPECT_EQ(B.getReceivedSize(), totalSent);
    EXPECT_EQ(B.getReceivedMessageCount(), totalSentCount);
    EXPECT_LE(_timestamp - startTime, 34 * utils::Time::sec);
}

TEST_F(SctpTransferTestFixture, withLoss20)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 2000);
    SctpEndpoint B(5001, _config, _timestamp, 2000);

    A._sendQueue.setLossRate(0.01);
    establishConnection(A, B);

    const int DATA_SIZE = 450 * 1024;
    std::array<uint8_t, DATA_SIZE> data;
    std::memset(data.data(), 0xcd, DATA_SIZE);
    const auto startTime = _timestamp;
    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_BINARY,
        data.data(),
        DATA_SIZE,
        _timestamp);
    const auto endTime = _timestamp + 4 * utils::Time::sec;
    for (int i = 0; static_cast<int64_t>(endTime - _timestamp) > 0 && B.getReceivedSize() == 0; ++i)
    {
        _timestamp += 100 * utils::Time::us;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }
    utils::Time::nanoSleep(500 * utils::Time::ms);
    EXPECT_EQ(B.getReceivedSize(), DATA_SIZE);
    logger::info("transmission time %" PRIu64 " ms", "", (_timestamp - startTime) / utils::Time::ms);
    EXPECT_LE(_timestamp - startTime, 3000 * utils::Time::ms);
}

TEST_F(SctpTransferTestFixture, mtuDiscovery)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);

    establishConnection(A, B);
    const size_t MTUA = 1250 + A._sendQueue.IPOVERHEAD;
    B._sendQueue.setMTU(MTUA);
    B._session->startMtuProbing(_timestamp);
    for (int i = 0; i < 100; ++i)
    {
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.forwardWhenReady(B);
        B.forwardWhenReady(A);
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.process();
        B.process();
    }

    EXPECT_EQ(B._session->getScptMTU(), 1152);
}

// checks that full cwnd does not prevent data flow
// from restarting after a number of lost sacks
TEST_F(SctpTransferTestFixture, lostSacks)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);
    SctpEndpoint blackhole(9000, _config, _timestamp, 250);

    establishConnection(A, B);

    const int DATA_SIZE = 450 * 1024;
    std::array<uint8_t, DATA_SIZE> data;
    std::memset(data.data(), 0xcd, DATA_SIZE);
    // const auto startTime = _timestamp;
    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_BINARY,
        data.data(),
        DATA_SIZE,
        _timestamp);
    for (int i = 0; i < 10000; ++i)
    {
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.forwardWhenReady(B);
        if (i > 10 && i < 600)
        {
            B.forwardWhenReady(blackhole);
        }
        else
        {
            B.forwardWhenReady(A);
        }
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.process();
        B.process();
    }

    EXPECT_EQ(B.getReceivedSize(), DATA_SIZE);
}

TEST_F(SctpTransferTestFixture, zeroRecvWindow)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);
    SctpEndpoint blackhole(9000, _config, _timestamp, 250);

    establishConnection(A, B);

    const int DATA_SIZE = 450 * 1024;
    std::array<uint8_t, DATA_SIZE> data;
    std::memset(data.data(), 0xcd, DATA_SIZE);

    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_BINARY,
        data.data(),
        DATA_SIZE,
        _timestamp);
    for (int i = 0; i < 10000; ++i)
    {
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.forwardWhenReady(B);
        if (i == 20)
        {
            B._session->setAdvertisedReceiveWindow(0);
        }
        else if (i == 200)
        {
            B._session->setAdvertisedReceiveWindow(128000);
        }
        else if (i > 150 && i < 200)
        {
            EXPECT_LT(A._sendQueue.count(), 2);
        }

        B.forwardWhenReady(A);
        ++_timestamp;
        _timestamp += std::min(A.getTimeout(), B.getTimeout());
        A.process();
        B.process();
    }

    EXPECT_EQ(B.getReceivedSize(), DATA_SIZE);
}

TEST_F(SctpTransferTestFixture, sendEmptyMessage)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);

    establishConnection(A, B);

    const auto sentFromA = A.sentPacketCount;
    const int DATA_SIZE = 250 * 1024;
    std::array<uint8_t, 1450> data;
    std::memset(data.data(), 0xdd, data.size());
    const auto startTime = _timestamp;
    size_t totalSent = 0;
    size_t totalSentCount = 0;
    const bool sendResult =
        A._session->sendMessage(A.getStreamId(), webrtc::DataChannelPpid::WEBRTC_STRING, data.data(), 0, _timestamp);
    EXPECT_TRUE(sendResult);

    for (int i = 0; i < 30000 && B.getReceivedSize() < DATA_SIZE; ++i)
    {
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }
    utils::Time::nanoSleep(100 * utils::Time::ms);
    EXPECT_EQ(A.sentPacketCount, sentFromA);
    EXPECT_EQ(B.getReceivedSize(), totalSent);
    EXPECT_EQ(B.getReceivedMessageCount(), totalSentCount);
    EXPECT_LE(_timestamp - startTime, 34 * utils::Time::sec);
}

TEST_F(SctpTransferTestFixture, sendMtuMessage)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);

    establishConnection(A, B);

    const auto sentFromA = A.sentPacketCount;
    const int DATA_SIZE = 250 * 1024;
    const size_t mtu = 996;
    std::array<uint8_t, mtu * 2> data;
    std::memset(data.data(), 0xdd, data.size());

    const bool sendResult =
        A._session->sendMessage(A.getStreamId(), webrtc::DataChannelPpid::WEBRTC_STRING, data.data(), mtu, _timestamp);
    EXPECT_TRUE(sendResult);

    for (int i = 0; i < 3000 && B.getReceivedSize() < mtu; ++i)
    {
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }
    utils::Time::nanoSleep(100 * utils::Time::ms);
    EXPECT_EQ(A.sentPacketCount, sentFromA + 1);
    EXPECT_EQ(B.getReceivedSize(), mtu);
    EXPECT_EQ(B.getReceivedMessageCount(), 1);

    EXPECT_TRUE(A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_STRING,
        data.data(),
        mtu * 2,
        _timestamp));

    for (int i = 0; i < 3000 && B.getReceivedSize() < DATA_SIZE; ++i)
    {
        _timestamp += 1 * utils::Time::ms;
        A.process();
        B.process();
        A.forwardPackets(B);
        B.forwardPackets(A);
    }
    utils::Time::nanoSleep(100 * utils::Time::ms);
    EXPECT_EQ(A.sentPacketCount, sentFromA + 3);
    EXPECT_EQ(B.getReceivedSize(), mtu * 3);
    EXPECT_EQ(B.getReceivedMessageCount(), 2);
}

class DummySctpTransport : public webrtc::DataStreamTransport
{
public:
    struct SctpInfo
    {
        uint16_t streamId = 0;
        uint32_t protocol = 0;
        char data[256];
        uint16_t length = 0;
    };
    DummySctpTransport() : _loggableId("dummy") {}

    bool sendSctp(uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length) override
    {
        SctpInfo info{streamId, protocolId};
        info.length = length;
        std::strncpy(info.data, reinterpret_cast<const char*>(data), length);
        _sendQueue.push(info);
        return true;
    }
    uint16_t allocateOutboundSctpStream() override { return 0; }

    logger::LoggableId _loggableId;

    std::queue<SctpInfo> _sendQueue;
};

TEST_F(SctpTransferTestFixture, sctpReorder)
{
    using namespace sctptest;
    SctpEndpoint A(5000, _config, _timestamp, 250);
    SctpEndpoint B(5001, _config, _timestamp, 250);
    establishConnection(A, B);

    DummySctpTransport fakeTransport;
    webrtc::WebRtcDataStream dataStream(2, fakeTransport, B.getAllocator());
    alignas(memory::Packet) const char webRtcOpen[] =
        "\x03\x00\x00\x00\x00\x00\x00\x00\x00\x12\x00\x00\x77\x65\x62\x72"
        "\x74\x63\x2d\x64\x61\x74\x61\x63\x68\x61\x6e\x6e\x65\x6c\x00\x00";

    alignas(memory::Packet) const char msg1[] = "\x7b\x22\x63\x6f\x6c\x69\x62\x72\x69\x43\x6c\x61\x73\x73\x22\x3a"
                                                "\x22\x53\x65\x6c\x65\x63\x74\x65\x64\x45\x6e\x64\x70\x6f\x69\x6e"
                                                "\x74\x73\x43\x68\x61\x6e\x67\x65\x64\x45\x76\x65\x6e\x74\x22\x2c"
                                                "\x22\x73\x65\x6c\x65\x63\x74\x65\x64\x45\x6e\x64\x70\x6f\x69\x6e"
                                                "\x74\x73\x22\x3a\x5b\x22\x64\x34\x31\x65\x33\x31\x35\x64\x2d\x33"
                                                "\x61\x62\x36\x2d\x34\x64\x32\x38\x2d\x62\x65\x36\x35\x2d\x39\x30"
                                                "\x33\x65\x39\x38\x63\x62\x37\x65\x66\x32\x22\x5d\x7d\x00\x00\x00";

    alignas(memory::Packet) const char msg2[] = "\x7b\x22\x63\x6f\x6c\x69\x62\x72\x69\x43\x6c\x61\x73\x73\x22\x3a"
                                                "\x22\x50\x69\x6e\x6e\x65\x64\x45\x6e\x64\x70\x6f\x69\x6e\x74\x73"
                                                "\x43\x68\x61\x6e\x67\x65\x64\x45\x76\x65\x6e\x74\x22\x2c\x22\x70"
                                                "\x69\x6e\x6e\x65\x64\x45\x6e\x64\x70\x6f\x69\x6e\x74\x73\x22\x3a"
                                                "\x5b\x22\x64\x34\x31\x65\x33\x31\x35\x64\x2d\x33\x61\x62\x36\x2d"
                                                "\x34\x64\x32\x38\x2d\x62\x65\x36\x35\x2d\x39\x30\x33\x65\x39\x38"
                                                "\x63\x62\x37\x65\x66\x32\x22\x5d\x7d\x00\x00\x00";

    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen,
        sizeof(webRtcOpen) - 1,
        _timestamp);
    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_STRING,
        msg1,
        sizeof(msg1) - 1,
        _timestamp);
    A._session->sendMessage(A.getStreamId(),
        webrtc::DataChannelPpid::WEBRTC_STRING,
        msg2,
        sizeof(msg2) - 1,
        _timestamp);
    A.process();
    auto* sctpOpen = A._sendQueue.pop();
    auto* sctpMsg1 = A._sendQueue.pop();
    auto* sctpMsg2 = A._sendQueue.pop();

    B._port->onPacketReceived(sctpMsg1->get(), sctpMsg1->getLength(), _timestamp);
    EXPECT_EQ(B.getReceivedMessageCount(), 0);
    B._port->onPacketReceived(sctpMsg2->get(), sctpMsg2->getLength(), _timestamp);
    EXPECT_EQ(B.getReceivedMessageCount(), 0);

    _timestamp += B.process();
    auto* sack1 = B._sendQueue.pop();
    auto* sack2 = B._sendQueue.pop();

    EXPECT_NE(sack1, nullptr);
    EXPECT_NE(sack2, nullptr);
    _timestamp += B.process();
    _timestamp += 500 * utils::Time::ms;
    B._port->onPacketReceived(sctpOpen->get(), sctpOpen->getLength(), _timestamp);
    EXPECT_EQ(B.getReceivedMessageCount(), 3);
    _timestamp += B.process();
    _timestamp += B.process();
    auto* sack3 = B._sendQueue.pop();

    dataStream.onSctpMessage(&fakeTransport, 0, 55, 0x32, sctpOpen->get() + 28, sctpOpen->getLength() - 28);
    auto openAckMsg = fakeTransport._sendQueue.front();
    fakeTransport._sendQueue.pop();
    B._session->sendMessage(A.getStreamId(), openAckMsg.protocol, openAckMsg.data, openAckMsg.length, _timestamp);
    B.process();
    auto* openAck = B._sendQueue.pop();
    EXPECT_NE(sack3, nullptr);
    EXPECT_NE(openAck, nullptr);

    A.getAllocator().free(sctpOpen);
    A.getAllocator().free(sctpMsg1);
    A.getAllocator().free(sctpMsg2);

    B.getAllocator().free(sack1);
    B.getAllocator().free(sack2);
    B.getAllocator().free(sack3);
    B.getAllocator().free(openAck);
}
