
#include "jobmanager/WorkerThread.h"
#include "test/transport/TransportIntegrationTest.h"
#include "transport/RtcTransport.h"
#include "transport/TransportFactory.h"
#include "webrtc/DataChannel.h"
#include "webrtc/WebRtcDataStream.h"
#include "gmock/gmock-matchers.h"
#include <cstdint>
#include <gtest/gtest.h>

using namespace std;
using namespace jobmanager;
using namespace concurrency;
using namespace transport;
using namespace testing;

struct SctpIntegrationTest : public TransportIntegrationTest
{
    SctpIntegrationTest() {}
};

namespace
{
std::string sctpMessage = "fairly ok message but nothing special. fairly ok message but nothing special.";
struct ClientPair : public TransportClientPair
{
    ClientPair(TransportFactory& transportFactory1,
        TransportFactory& transportFactory2,
        uint32_t ssrc,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::SslDtls& sslDtls,
        JobManager& jobManager,
        bool blockUdp)
        : TransportClientPair(transportFactory1,
              transportFactory2,
              ssrc,
              allocator,
              audioAllocator,
              sslDtls,
              jobManager,
              blockUdp),
          _stream1(_transport1->getId(), *_transport1, _sendAllocator),
          _stream2(_transport2->getId(), *_transport2, _sendAllocator)
    {
        _messagesReceived[0] = 0;
        _messagesReceived[1] = 0;
        _transport1->setSctp(5000, 5000);
        _transport2->setSctp(5000, 5000);
    }

    ~ClientPair() {}

    void establishSctp() { _transport1->connectSctp(); }

    void onSctpEstablished(transport::RtcTransport* sender) override
    {
        logger::info("sctp established", sender->getLoggableId().c_str());
        ++_sctpEstablished;
    }

    void onSctpMessage(RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override
    {
        if (payloadProtocol != webrtc::DataChannelPpid::WEBRTC_STRING)
        {
            if (sender == _transport1.get())
            {
                _stream1.onSctpMessage(sender, streamId, streamSequenceNumber, payloadProtocol, data, length);
            }
            else
            {
                _stream2.onSctpMessage(sender, streamId, streamSequenceNumber, payloadProtocol, data, length);
            }
        }

        if (payloadProtocol == webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
        {
            // create command with this packet to send the binary data -> engine -> WebRtcDataStream belonging to this
            // transport
            logger::info("webrtc Data channel established %u", sender->getLoggableId().c_str(), streamId);

            return;
        }
        else if (payloadProtocol == webrtc::DataChannelPpid::WEBRTC_STRING)
        {
            std::string body(reinterpret_cast<const char*>(data), length);
            EXPECT_EQ(body, sctpMessage);
            if (sender == _transport1.get())
            {
                ++_messagesReceived[0];
            }
            else
            {
                ++_messagesReceived[1];
            }
        }
    }

    webrtc::WebRtcDataStream _stream1;
    webrtc::WebRtcDataStream _stream2;
    std::atomic<int> _sctpEstablished = {0};
    std::atomic<uint32_t> _messagesReceived[2];
};
} // namespace

TEST_F(SctpIntegrationTest, connectUdp)
{
    ASSERT_TRUE(_transportFactory1->isGood());
    ASSERT_TRUE(_transportFactory2->isGood());

    ClientPair clients(*_transportFactory1,
        *_transportFactory2,
        11001u,
        *_mainPoolAllocator,
        _audioAllocator,
        *_sslDtls,
        *_jobManager,
        false);

    const auto startTime = utils::Time::getAbsoluteTime();
    clients.connect(startTime);

    while (!clients.isConnected() && utils::Time::getAbsoluteTime() - startTime < 2 * utils::Time::sec)
    {
        utils::Time::nanoSleep(100 * utils::Time::ms);
        clients.tryConnect(utils::Time::getAbsoluteTime(), *_sslDtls);
    }

    while (clients._sctpEstablished < 2 && utils::Time::getAbsoluteTime() - startTime < 2 * utils::Time::sec)
    {
        utils::Time::nanoSleep(100 * utils::Time::ms);
    }
    ASSERT_EQ(clients._sctpEstablished, 2);

    webrtc::WebRtcDataStream stream1(clients._transport1->getId(), *clients._transport1, clients._sendAllocator);
    stream1.open("fastone");

    while (clients._sctpEstablished < 2 && utils::Time::getAbsoluteTime() - startTime < 2 * utils::Time::sec)
    {
        utils::Time::nanoSleep(100 * utils::Time::ms);
    }
    ASSERT_EQ(clients._sctpEstablished, 2);

    const uint32_t messageCount = (__has_feature(thread_sanitizer) ? 10 : 8800);
    for (uint32_t i = 0; i < messageCount; ++i)
    {
        stream1.sendString(sctpMessage.c_str(), sctpMessage.size());
        utils::Time::nanoSleep(200 * utils::Time::us);
        if ((i % 1000) == 0)
        {
            logger::debug("received %u", "sctpmany", clients._messagesReceived[1].load());
        }
    }
    utils::Time::nanoSleep(1000 * utils::Time::ms);
    EXPECT_EQ(messageCount, clients._messagesReceived[1]);
    EXPECT_EQ(0, clients._messagesReceived[0]);
    EXPECT_TRUE(clients.isConnected());
    clients.stop();
}
