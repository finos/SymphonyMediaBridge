
#include "jobmanager/WorkerThread.h"
#include "test/macros.h"
#include "test/transport/TransportIntegrationTest.h"
#include "transport/RtcSocket.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "gmock/gmock-matchers.h"
#include <cstdint>
#include <gtest/gtest.h>

using namespace std;
using namespace jobmanager;
using namespace concurrency;
using namespace transport;
using namespace testing;

struct IceIntegrationTest : public TransportIntegrationTest
{
    IceIntegrationTest() {}
};

namespace
{
struct ClientPair : public TransportClientPair
{
    ClientPair(TransportFactory& transportFactory1,
        TransportFactory& transportFactory2,
        uint32_t ssrc,
        memory::PacketPoolAllocator& allocator,
        transport::SslDtls& sslDtls,
        JobManager& jobManager,
        bool blockUdp)
        : TransportClientPair(transportFactory1, transportFactory2, ssrc, allocator, sslDtls, jobManager, blockUdp){};

    ~ClientPair() {}
};
} // namespace

TEST_F(IceIntegrationTest, connectUdp)
{
    ASSERT_TRUE(_transportFactory1->isGood());
    ASSERT_TRUE(_transportFactory2->isGood());

    ClientPair
        clients(*_transportFactory1, *_transportFactory2, 11001u, *_mainPoolAllocator, *_sslDtls, *_jobManager, false);

    const auto startTime = utils::Time::getAbsoluteTime();
    clients.connect(startTime);

    while (!clients.isConnected() && utils::Time::getAbsoluteTime() - startTime < 2 * utils::Time::sec)
    {
        utils::Time::nanoSleep(100 * utils::Time::ms);
        clients.tryConnect(utils::Time::getAbsoluteTime(), *_sslDtls);
    }
    EXPECT_TRUE(clients.isConnected());
    clients.stop();
}

TEST_F(IceIntegrationTest, connectTcp)
{
    std::string configJson1 =
        "{\"rtc.ip\": \"127.0.0.1\", \"ice.singlePort\":10020, \"ice.tcp.enable\":true,\"ice.tcp.port\":14757}";
    std::string configJson2 =
        "{\"rtc.ip\": \"127.0.0.1\", \"ice.singlePort\":10010, \"ice.tcp.enable\":true, \"ice.tcp.port\":14447}";
    _config1.readFromString(configJson1);
    _config2.readFromString(configJson2);

    ASSERT_TRUE(init(configJson1, configJson2));
    {
        ClientPair clients(*_transportFactory1,
            *_transportFactory2,
            11001u,
            *_mainPoolAllocator,
            *_sslDtls,
            *_jobManager,
            true);

        const auto startTime = utils::Time::getAbsoluteTime();
        clients.connect(startTime);

        while (!clients.isConnected() &&
            utils::Time::diffLT(startTime, utils::Time::getAbsoluteTime(), 15 * utils::Time::sec))
        {
            utils::Time::nanoSleep(100 * utils::Time::ms);
            clients.tryConnect(utils::Time::getAbsoluteTime(), *_sslDtls);
        }
        EXPECT_TRUE(clients.isConnected());
        clients.stop();
    }
}

TEST_F(IceIntegrationTest, connectTcpAlias)
{
    std::string configJson1 = "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10020, "
                              "\"ice.tcp.enable\":true,\"ice.tcp.port\":14768, \"ice.tcp.aliasPort\":15001, "
                              "\"ice.publicIpv4\":\"192.168.1.239\"}";
    std::string configJson2 = "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10044, \"ice.tcp.enable\":true, "
                              "\"ice.tcp.port\":14868, \"ice.tcp.aliasPort\":15011}";
    _config1.readFromString(configJson1);

    _iceConfig.publicIpv4 = transport::SocketAddress::parse(_config1.ice.publicIpv4);
    ASSERT_TRUE(init(configJson1, configJson2));
    {
        ClientPair clients(*_transportFactory1,
            *_transportFactory2,
            11001u,
            *_mainPoolAllocator,
            *_sslDtls,
            *_jobManager,
            true);

        auto candidates1 = clients._transport1->getLocalCandidates();
        EXPECT_EQ(candidates1.size(), 5);
        auto aliasIt = std::find_if(candidates1.cbegin(), candidates1.cend(), [](const ice::IceCandidate& item) {
            return item.address == transport::SocketAddress::parse("192.168.1.239", 15001);
        });
        EXPECT_NE(candidates1.cend(), aliasIt);
        clients.stop();
    }
}

struct AttackListener : transport::RtcePoll::IEventListener
{
public:
    AttackListener() : disconnects(0), stopCount(0), startedCount(0), readableCount(0), writeableCount(0) {}

    void onSocketPollStarted(int fd) override { ++startedCount; }
    void onSocketPollStopped(int fd) override { ++stopCount; }
    void onSocketReadable(int fd) override { ++readableCount; }
    void onSocketWriteable(int fd) override { ++writeableCount; }
    void onSocketShutdown(int fd) override { ++disconnects; }

    std::atomic<uint32_t> disconnects;
    std::atomic<uint32_t> stopCount;
    std::atomic<uint32_t> startedCount;
    std::atomic<uint32_t> readableCount;
    std::atomic<uint32_t> writeableCount;
};

TEST_F(IceIntegrationTest, dosAttackTcpConnect)
{
    std::string configJson1 =
        "{\"rtc.ip\": \"127.0.0.1\", \"ice.singlePort\":10020, \"ice.tcp.enable\":true,\"ice.tcp.port\":14768}";
    std::string configJson2 = "{\"rtc.ip\": \"127.0.0.1\", \"ice.singlePort\":10010, \"ice.tcp.enable\":false}";
    _config1.readFromString(configJson1);
    _config2.readFromString(configJson2);

    ASSERT_TRUE(init(configJson1, configJson2));

    ClientPair
        clients(*_transportFactory1, *_transportFactory2, 11001u, *_mainPoolAllocator, *_sslDtls, *_jobManager, true);

    AttackListener attackListener;
    std::vector<transport::RtcSocket*> clientSockets;
    for (int i = 0; i < 470; ++i)
    {
        clientSockets.push_back(new transport::RtcSocket());
        auto socket = clientSockets.back();
        if (0 != socket->open(SocketAddress::parse("127.0.0.1"), 0, SOCK_STREAM))
        {
            EXPECT_TRUE(false);
            break;
        }
        _network->add(socket->fd(), &attackListener);
    }

    for (int i = 0; i < 200 && attackListener.startedCount < clientSockets.size(); ++i)
    {
        utils::Time::nanoSleep(utils::Time::ms * 10);
    }
    ASSERT_EQ(attackListener.startedCount, clientSockets.size());
    attackListener.disconnects = 0; // a lot of disconnects will be signalled when socket opens in unconnected state
    attackListener.writeableCount = 0;
    int count = 0;
    for (auto* socket : clientSockets)
    {
        if (0 != socket->connect(SocketAddress::parse("127.0.0.1", _config1.ice.tcp.port)))
        {
            EXPECT_TRUE(false);
            break;
        }

        // if we connect faster the connections will be silently dropped in accept queue
        utils::Time::nanoSleep(utils::Time::ms * 2);
        if (++count == 450)
        {
            EXPECT_EQ(attackListener.readableCount, 0); // none disconnected yet
        }
    }

    // wait for at least 460 sockets to be accepted
    for (int i = 0; i < 300 && attackListener.writeableCount < 460; ++i)
    {
        utils::Time::nanoSleep(utils::Time::ms * 20);
    }
    logger::debug("attack counters disconnects %u, writeable %u, readable %u",
        "",
        attackListener.disconnects.load(),
        attackListener.writeableCount.load(),
        attackListener.readableCount.load());

    // apple will signal as disconnect, linux will signal as readable
    for (int i = 0; i < 300 && attackListener.disconnects + attackListener.readableCount < 460; ++i)
    {
        utils::Time::nanoSleep(utils::Time::ms * 20);
    }

    logger::debug("attack counters disconnects %u, writeable %u, readable %u",
        "",
        attackListener.disconnects.load(),
        attackListener.writeableCount.load(),
        attackListener.readableCount.load());
#ifdef __APPLE__
    EXPECT_EQ(attackListener.disconnects, clientSockets.size());
#else
    EXPECT_GE(attackListener.readableCount, 460);
#endif
    count = 0;
    for (auto* clientSocket : clientSockets)
    {
        uint8_t buf[8];
        auto rc = ::recv(clientSocket->fd(), buf, sizeof(buf), MSG_DONTWAIT);

        EXPECT_THAT(rc, AnyOf(0, -1));
    }
    utils::Time::nanoSleep(utils::Time::ms * 100); // allow close signals to arrive

    for (auto socket : clientSockets)
    {
        _network->remove(socket->fd(), &attackListener);
    }
    while (attackListener.stopCount < clientSockets.size())
    {
        utils::Time::nanoSleep(utils::Time::ms * 10);
    }
    for (auto socket : clientSockets)
    {
        socket->close();
        delete socket;
    }
    clients.stop();
}

TEST_F(IceIntegrationTest, dtlsRace)
{
    ASSERT_TRUE(_transportFactory1->isGood());
    ASSERT_TRUE(_transportFactory2->isGood());

    std::vector<ClientPair*> clients;
    for (int i = 0; i < 100; ++i)
    {
        clients.push_back(new ClientPair(*_transportFactory1,
            *_transportFactory2,
            11001u + i * 2,
            *_mainPoolAllocator,
            *_sslDtls,
            *_jobManager,
            false));
    }

    const auto startTime = utils::Time::getAbsoluteTime();
    for (auto* clientPair : clients)
    {
        clientPair->connect(startTime);
    }

    bool connected = false;
    while (!connected && utils::Time::getAbsoluteTime() - startTime < 5 * utils::Time::sec)
    {
        int64_t maxSleep = utils::Time::sec;

        connected = true;
        for (auto* clientPair : clients)
        {
            maxSleep = std::min(maxSleep, clientPair->tryConnect(utils::Time::getAbsoluteTime(), *_sslDtls));
            if (!clientPair->isConnected())
            {
                connected = false;
            }
        }
        utils::Time::nanoSleep(maxSleep);
    }

    int connectCount = 0;
    for (auto* clientPair : clients)

    {
        if (clientPair->isConnected())
        {
            ++connectCount;
        }
    }
    EXPECT_TRUE(connected);
    EXPECT_EQ(connectCount, clients.size());
    for (auto* clientPair : clients)
    {
        clientPair->stop();
        delete clientPair;
    }
}

TEST_F(IceIntegrationTest, portReuse)
{
    // Connect two sessions from 10010 to 10030. Since client ports have to be unique,
    // the second session should take over and DTLS can succeed in second session
    // Mimics the case where an RTT probe is made and left unconnected,
    // then the firewall reuses the port for another session that should be able to
    // replace the first unfinished session.
    ASSERT_TRUE(init("{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10030}",
        "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10010}"));

    ClientPair* clients1 = new ClientPair(*_transportFactory1,
        *_transportFactory2,
        11001u,
        *_mainPoolAllocator,
        *_sslDtls,
        *_jobManager,
        false);
    ClientPair* clients2 = new ClientPair(*_transportFactory1,
        *_transportFactory2,
        11009u,
        *_mainPoolAllocator,
        *_sslDtls,
        *_jobManager,
        false);

    clients1->connect(utils::Time::getAbsoluteTime());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    clients2->connect(utils::Time::getAbsoluteTime());
    for (int i = 0; !clients2->isConnected() && i < 300; ++i)
    {
        clients2->tryConnect(utils::Time::getAbsoluteTime(), *_sslDtls);
        utils::Time::nanoSleep(10 * utils::Time::ms);
    }

    if (!__has_feature(thread_sanitizer))
    {
        EXPECT_TRUE(clients2->isConnected());
        EXPECT_FALSE(clients1->isConnected());
    }

    clients1->stop();
    clients2->stop();

    delete clients1;
    delete clients2;
}
