#include "FakeNetwork.h"
#include "crypto/SslHelper.h"
#include "jobmanager/JobManager.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "transport/RtcSocket.h"
#include "transport/RtcePoll.h"
#include "transport/ice/IceSession.h"
#include "transport/ice/Stun.h"
#include <atomic>
#include <gtest/gtest.h>

namespace
{
void setRemoteCandidates(ice::IceSession& target, ice::IceSession& source)
{
    for (auto candidate : source.getLocalCandidates())
    {
        candidate.baseAddress = transport::SocketAddress();
        target.addRemoteCandidate(candidate);
    }
}
} // namespace
TEST(IceTest, utf8)
{
    ice::StunMessage stun;
    ice::StunTransactionIdGenerator gen;

    stun.header.transactionId.set(gen.next());
    ice::StunXorMappedAddress addr;
    addr.setAddress(transport::SocketAddress::parse("10.45.1.121", 21000), stun.header);
    stun.add(addr);

    ice::StunGenericAttribute software(ice::StunAttribute::SOFTWARE);
    software.setValue("Symphony Mixer");
    stun.add(software);
    EXPECT_EQ(software.getUtf8(), "Symphony Mixer");
}

TEST(IceTest, parseprobe)
{
    using namespace ice;
    alignas(memory::Packet) const char* raw = "\x01\x01\x00\x3c\x21\x12\xa4\x42"
                                              "\x71\x6c\x0a\xcc\x6e\x50\x2a\x9e"
                                              "\xf7\xdf\x3c\xf2\x00\x20\x00\x08\x00\x01"
                                              "\x97\xd2\xe1\xba\xa5\xac\x00\x06\x00\x09"
                                              "\x59\x45\x31\x75\x3a\x6d\x52\x44\x38\x20"
                                              "\x20\x20\x00\x08\x00\x14\xf4\x70\x2d\x9a"
                                              "\x96\x83\xd6\x34\x67\xe6\xee\x7e\x75\xcc"
                                              "\x12\x2b\x16\xcc\x81\x74\x80\x28\x00\x04"
                                              "\x9c\x15\xa1\x33";
    auto stun = StunMessage::fromPtr(raw);

    EXPECT_EQ(stun->header.transactionId.get(), (__uint128_t(0x716c0acc6e502a9eull) << 4 * 8) | 0xf7df3cf2ull);
    EXPECT_EQ(stun->header.length, 60);
    EXPECT_EQ(stun->header.method.get(), ice::StunHeader::BindingResponse);

    auto it = stun->cbegin();
    {
        auto& attr = *it;
        EXPECT_EQ(attr.type, StunAttribute::XOR_MAPPED_ADDRESS);
        EXPECT_EQ(attr.length, 8);
        auto xorMapAddr = reinterpret_cast<const StunXorMappedAddress&>(attr);
        EXPECT_EQ(xorMapAddr.getFamily(), StunXorMappedAddress::Family_v4);
        auto addr = xorMapAddr.getAddress(stun->header);
        EXPECT_EQ(addr.getPort(), 46784);
        EXPECT_EQ(addr.ipToString(), "192.168.1.238");
    }
    ++it;
    {
        auto& attr = *it;
        EXPECT_EQ(attr.type, StunAttribute::USERNAME);
        EXPECT_EQ(attr.length, 9);
        auto& gen = reinterpret_cast<const StunGenericAttribute&>(attr);
        EXPECT_EQ(gen.getUtf8(), "YE1u:mRD8");
    }
    ++it;
    {
        auto& attr = *it;
        EXPECT_EQ(attr.type, StunAttribute::MESSAGE_INTEGRITY);
        EXPECT_EQ(attr.length, 20);
        EXPECT_EQ(crypto::toHexString(attr.get(), attr.length), "f4702d9a9683d63467e6ee7e75cc122b16cc8174");
    }
    ++it;
    {
        auto fingerprintAttribute = reinterpret_cast<const StunFingerprint&>(*it);
        EXPECT_EQ(fingerprintAttribute.type, StunAttribute::FINGERPRINT);
        EXPECT_EQ(fingerprintAttribute.length, 4);

        uint32_t crc = fingerprintAttribute.value;
        EXPECT_EQ(crc, 0x9c15a133);
        EXPECT_EQ(stun->computeFingerprint(), crc);
    }
}

// test vector from RFC
TEST(IceTest, hmac1)
{
    using namespace ice;
    alignas(memory::Packet) const unsigned char req[] = "\x00\x01\x00\x58"
                                                        "\x21\x12\xa4\x42"
                                                        "\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae"
                                                        // software
                                                        "\x80\x22\x00\x10"
                                                        "\x53\x54\x55\x4e"
                                                        "\x20\x74\x65\x73"
                                                        "\x74\x20\x63\x6c"
                                                        "\x69\x65\x6e\x74"
                                                        // prio
                                                        "\x00\x24\x00\x04"
                                                        "\x6e\x00\x01\xff"
                                                        // ice controlled
                                                        "\x80\x29\x00\x08"
                                                        "\x93\x2f\xf9\xb1\x51\x26\x3b\x36"
                                                        // username
                                                        "\x00\x06\x00\x09"
                                                        "\x65\x76\x74\x6a\x3a\x68\x36\x76\x59\x20\x20\x20"
                                                        // msg integrity
                                                        "\x00\x08\x00\x14"
                                                        "\x9a\xea\xa7\x0c\xbf\xd8\xcb\x56\x78\x1e\xf2\xb5"
                                                        "\xb2\xd3\xf2\x49\xc1\xb5\x71\xa2"
                                                        // fingerprint
                                                        "\x80\x28\x00\x04"
                                                        "\xe5\x7a\x3b\xcf";

    std::string pwd = "VOkJxbRl1RmTxUk/WvJxBt";
    EXPECT_TRUE(ice::isStunMessage(req, sizeof(req) - 1));
    auto stun = ice::StunMessage::fromPtr(req);
    EXPECT_TRUE(stun->isValid());
    EXPECT_TRUE(stun->isAuthentic(pwd));
}

TEST(IceTest, jvbnice)
{
    using namespace ice;
    alignas(memory::Packet) const unsigned char reqFromJvb[] = "\x00\x01\x00\x5c\x21\x12\xa4\x42"
                                                               "\xc0\x53\x30\x7a\x6f\x01\x42\x72"
                                                               "\xb8\xeb\x57\xda\x00\x24\x00\x04"
                                                               "\x6e\x00\x0a\xff\x80\x29\x00\x08"
                                                               "\x3b\x1a\xaf\x34\xa8\x2f\x9b\x08"
                                                               "\x00\x06\x00\x13\x64\x4e\x52\x51"
                                                               "\x3a\x36\x6a\x70\x36\x38\x31\x64"
                                                               "\x74\x74\x33\x30\x6b\x72\x6f\x00"
                                                               "\x80\x22\x00\x09\x69\x63\x65\x34"
                                                               "\x6a\x2e\x6f\x72\x67\x00\x00\x00"
                                                               "\x00\x08\x00\x14\x56\xf5\xf2\x1c"
                                                               "\x30\xe9\x36\x3d\x50\xb4\x50\x9f"
                                                               "\x72\x6c\x5a\xa4\xc8\x78\xee\x15"
                                                               "\x80\x28\x00\x04\x5a\xde\x5f\x8e";
    alignas(memory::Packet) const unsigned char reqFromNice[] = "\x00\x01\x00\x50\x21\x12\xa4\x42"
                                                                "\xca\x15\xc2\x20\x36\xfe\x92\x40"
                                                                "\xb0\x61\xfc\x69\x00\x25\x00\x00"
                                                                "\x00\x24\x00\x04\x6e\x00\x01\xfe"
                                                                "\x80\x2a\x00\x08\xab\xe7\x8f\x56"
                                                                "\x8c\x8a\xaa\x67\x00\x06\x00\x13"
                                                                "\x36\x6a\x70\x36\x38\x31\x64\x74"
                                                                "\x74\x33\x30\x6b\x72\x6f\x3a\x64"
                                                                "\x4e\x52\x51\x20\x00\x08\x00\x14"
                                                                "\x86\x6e\x3b\xc6\xdc\xa3\xe8\x43"
                                                                "\xb0\x7c\x57\x22\x88\x96\xb0\xdb"
                                                                "\x43\x16\x26\xc3\x80\x28\x00\x04"
                                                                "\x0b\xfc\x50\xe7";

    alignas(memory::Packet) const unsigned char rspFromJvb[] = "\x01\x01\x00\x54\x21\x12\xa4\x42"
                                                               "\xca\x15\xc2\x20\x36\xfe\x92\x40"
                                                               "\xb0\x61\xfc\x69\x00\x20\x00\x08"
                                                               "\x00\x01\x43\x41\xe1\xba\xa5\x43"
                                                               "\x00\x06\x00\x13\x36\x6a\x70\x36"
                                                               "\x38\x31\x64\x74\x74\x33\x30\x6b"
                                                               "\x72\x6f\x3a\x64\x4e\x52\x51\x00"
                                                               "\x80\x22\x00\x09\x69\x63\x65\x34"
                                                               "\x6a\x2e\x6f\x72\x67\x00\x00\x00"
                                                               "\x00\x08\x00\x14\x39\x06\x27\x3d"
                                                               "\x8f\x75\x94\x7a\x5d\x5c\x59\x64"
                                                               "\xe6\x2b\x6e\xd6\xb4\xe0\x6a\xb4"
                                                               "\x80\x28\x00\x04\x33\x92\xec\xfe";

    alignas(memory::Packet) const unsigned char rspFromNice[] = "\x01\x01\x00\x44\x21\x12\xa4\x42"
                                                                "\xc0\x53\x30\x7a\x6f\x01\x42\x72"
                                                                "\xb8\xeb\x57\xda\x00\x20\x00\x08"
                                                                "\x00\x01\x06\x43\xe1\xba\xa5\xac"
                                                                "\x00\x06\x00\x13\x64\x4e\x52\x51"
                                                                "\x3a\x36\x6a\x70\x36\x38\x31\x64"
                                                                "\x74\x74\x33\x30\x6b\x72\x6f\x20"
                                                                "\x00\x08\x00\x14\x1e\x73\x04\xc7"
                                                                "\x81\x2d\x04\xed\xce\x08\xf8\x69"
                                                                "\x21\x8b\x63\x29\x50\xfe\x27\x34"
                                                                "\x80\x28\x00\x04\xa2\x32\x42\xe4";

    // outbound leg, multi leg
    const char* jvbUser1 = "6jp681dtt30kro";
    const char* jvbPwd1 = "3s8tg4f159kdj7siunb3p9p248";
    const char* niceUser2 = "dNRQ";
    const char* nicePwd2 = "g1TyvQ7V0N8uCmpKJazelE";

    {
        auto reqJvb = StunMessage::fromPtr(reqFromJvb);
        EXPECT_TRUE(reqJvb->isValid());
        EXPECT_TRUE(reqJvb->isAuthentic(nicePwd2));
        auto user = reqJvb->getAttribute<ice::StunGenericAttribute>(ice::StunAttribute::USERNAME);
        EXPECT_EQ(user->getUtf8(), std::string(niceUser2) + ":" + std::string(jvbUser1));
    }
    {
        auto rspNice = StunMessage::fromPtr(rspFromNice);
        EXPECT_TRUE(rspNice->isAuthentic(nicePwd2));
        EXPECT_TRUE(rspNice->isValid());
        auto user = rspNice->getAttribute<ice::StunGenericAttribute>(ice::StunAttribute::USERNAME);
        EXPECT_EQ(user->getUtf8(), std::string(niceUser2) + ":" + std::string(jvbUser1));
    }
    {
        auto reqNice = StunMessage::fromPtr(reqFromNice);
        EXPECT_TRUE(reqNice->isAuthentic(jvbPwd1));
        auto user = reqNice->getAttribute<ice::StunGenericAttribute>(ice::StunAttribute::USERNAME);
        EXPECT_EQ(user->getUtf8(), std::string(jvbUser1) + ":" + std::string(niceUser2));
    }
    {
        auto rspJvb = StunMessage::fromPtr(rspFromJvb);
        EXPECT_TRUE(rspJvb->isAuthentic(jvbPwd1));
        auto user = rspJvb->getAttribute<ice::StunGenericAttribute>(ice::StunAttribute::USERNAME);
        EXPECT_EQ(user->getUtf8(), std::string(jvbUser1) + ":" + std::string(niceUser2));
    }
}

TEST(IceTest, hmac2)
{
    using namespace ice;
    alignas(memory::Packet) const unsigned char req[] =
        "\x00\x01\x00\x50\x21\x12\xa4\x42"
        "\xa5\x89\xa1\x52\x2c\xf6\xd6\x1f"
        "\x12\xf2\x55\x69\x00\x06\x00\x11"
        "\x36\x6b\x31\x68\x68\x32\x67\x64"
        "\x3a\x38\x62\x63\x31\x64\x62\x61"
        "\x34\x00\x00\x00\x00\x25\x00\x00"
        "\x00\x24\x00\x04\x6e\x7f\x00\xff"
        "\x80\x2a\x00\x08\xe4\x53\x2a\x52"
        "\xba\x06\xbd\xe9"
        "\x00\x08\x00\x14\xbd\xe0\xf2\x7b\xf7\xd8\x71\x70"
        "\x67\x44\x86\x9a\xa2\xd9\xa3\xa0\x27\xd6\xa9\xd1\x80\x28\x00\x04"
        "\x04\x36\x14\x1e";

    std::string pwd = "fpllngzieyoh43e0133ols";
    auto stun = ice::StunMessage::fromPtr(req);
    EXPECT_TRUE(ice::isStunMessage(req, sizeof(req) - 1));
    EXPECT_TRUE(stun->isValid());
    EXPECT_TRUE(stun->isAuthentic(pwd));
}

TEST(IceTest, ipformat)
{
    using namespace transport;
    auto a = SocketAddress::parse("192.10.14.231");
    EXPECT_EQ(a.toString(), "192.10.14.231");
    EXPECT_EQ(a.getPort(), 0);

    auto b = SocketAddress::parse("192.10.14.232", 443);
    EXPECT_EQ(b.ipToString(), "192.10.14.232");
    EXPECT_EQ(b.getPort(), 443);
    EXPECT_EQ(b.toString(), "192.10.14.232:443");

    auto c = SocketAddress::parse("fe80::1:3ba9:6b1f:f7f2", 443);
    EXPECT_EQ(c.ipToString(), "fe80::1:3ba9:6b1f:f7f2");
    EXPECT_EQ(c.getPort(), 443);
    EXPECT_EQ(c.toString(), "[fe80::1:3ba9:6b1f:f7f2]:443");
    c.setPort(0);
    EXPECT_EQ(c.toString(), "fe80::1:3ba9:6b1f:f7f2");
}

TEST(IceTest, linkLocal)
{
    using namespace transport;
    auto b = SocketAddress::parse("167.254.1.1", 4700);
    EXPECT_FALSE(b.isLinkLocal());
    auto a = SocketAddress::parse("169.254.6.7", 4700);
    EXPECT_TRUE(a.isLinkLocal());

    auto c = SocketAddress::parse("fe80::1:3456:1111", 4700);
    EXPECT_TRUE(c.isLinkLocal());

    auto d = SocketAddress::parse("fe21::1:3456:1111", 4700);
    EXPECT_FALSE(d.isLinkLocal());
}

TEST(IceTest, ipv6Response)
{
    alignas(memory::Packet) const unsigned char rsp[] = "\x01\x01\x00\x48" //     Response type and message length
                                                        "\x21\x12\xa4\x42" //     Magic cookie
                                                        "\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae" // transid
                                                        "\x80\x22\x00\x0b\x74\x65\x73\x74" // software
                                                        "\x20\x76\x65\x63"
                                                        "\x74\x6f\x72\x20"
                                                        // xor mapped address
                                                        "\x00\x20\x00\x14"
                                                        "\x00\x02\xa1\x47"
                                                        "\x01\x13\xa9\xfa"
                                                        "\xa5\xd3\xf1\x79"
                                                        "\xbc\x25\xf4\xb5"
                                                        "\xbe\xd2\xb9\xd9"
                                                        // integrity
                                                        "\x00\x08\x00\x14"
                                                        "\xa3\x82\x95\x4e"
                                                        "\x4b\xe6\x7b\xf1"
                                                        // fingerprint
                                                        "\x17\x84\xc9\x7c"
                                                        "\x82\x92\xc2\x75"
                                                        "\xbf\xe3\xed\x41"
                                                        "\x80\x28\x00\x04"
                                                        "\xc8\xfb\x0b\x4c";

    EXPECT_TRUE(ice::isStunMessage(rsp, sizeof(rsp) - 1));
    auto msg = ice::StunMessage::fromPtr(rsp);
    auto mappedAddress = msg->getAttribute<ice::StunXorMappedAddress>(ice::StunAttribute::XOR_MAPPED_ADDRESS);
    auto address = mappedAddress->getAddress(msg->header);
    auto expected = transport::SocketAddress::parse("2001:db8:1234:5678:11:2233:4455:6677", 32853);
    EXPECT_TRUE(expected == address);
}

TEST(IceTest, stunv6)
{
    ice::StunMessage msg;
    msg.header.setMethod(ice::StunHeader::BindingRequest);
    msg.header.transactionId.set(0x1111222233334444ull);
    ice::StunXorMappedAddress addr;
    auto address = transport::SocketAddress::parse("a000:1092:10cc:f56e::3c00", 0);
    addr.setAddress(address, msg.header);
    auto readAddress = addr.getAddress(msg.header);
    logger::info("read address %s", "", readAddress.toString().c_str());
    EXPECT_TRUE(address == readAddress);
}

TEST(IceTest, build)
{
    using namespace ice;
    ice::StunMessage msg;
    msg.header.setMethod(ice::StunHeader::BindingRequest);
    msg.header.transactionId.set(0x1111222233334444ull);

    uint32_t tieBreaker = 0x1234123;
    msg.add(StunGenericAttribute(StunAttribute::SOFTWARE, "slice"));
    msg.add(StunGenericAttribute(StunAttribute::USERNAME, "target:sender"));
    msg.add(StunControlled(tieBreaker));
    msg.add(StunPriority(912837490u));
    const char* pwd = "Hw89ty98masndbn";
    msg.addMessageIntegrity(pwd);
    msg.addFingerprint();
    EXPECT_TRUE(msg.isValid());
    EXPECT_TRUE(msg.isAuthentic(pwd));
}

class IceSocketAdapter : public ice::IceEndpoint
{
public:
    IceSocketAdapter() {}
    virtual ~IceSocketAdapter(){};

    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override
    {
        _socket.sendTo(static_cast<const char*>(data), len, target);
    }
    transport::SocketAddress getLocalPort() const override { return _ip; }
    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }
    transport::RtcSocket _socket;
    transport::SocketAddress _ip;
};

class IceTestInfra : public transport::RtcePoll::IEventListener
{
public:
    IceTestInfra(const ice::IceConfig& config)
        : session(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING, nullptr),
          _socketService(transport::createRtcePoll()),
          _socketCount(0),
          _inboundPackets(32){};

    void add(transport::SocketAddress localInterface)
    {
        sockets.emplace_back(new IceSocketAdapter());

        int rc = 1;
        for (int port = 11090; rc != 0 && port < 65500; ++port)
        {
            rc = sockets.back()->_socket.open(localInterface, port);
            if (rc == 0)
            {
                localInterface.setPort(port);
                sockets.back()->_ip = localInterface;
                session.attachLocalEndpoint(sockets.back().get());
                _socketService->add(sockets.back()->_socket.fd(), this);
                ++_socketCount;
            }
        }
    }

    void stop()
    {
        for (auto& socket : sockets)
        {
            _socketService->remove(socket->_socket.fd(), this);
        }
        while (_socketCount > 0)
        {
            utils::Time::nanoSleep(utils::Time::ms * 100);
        }
        _socketService->stop();
    }

    void onSocketPollStarted(int fd) override {}
    void onSocketShutdown(int fd) override {}
    void onSocketWriteable(int fd) override {}
    void onSocketPollStopped(int fd) override { --_socketCount; }
    void onSocketReadable(int fd) override
    {
        for (auto& s : sockets)
        {
            if (s->_socket.fd() == fd)
            {
                uint8_t data[1600];

                socklen_t addressSize = sizeof(sockaddr_in6);
                transport::RawSockAddress remoteAddress;

                auto count = recvfrom(s->_socket.fd(), data, 1600, MSG_DONTWAIT, &remoteAddress.gen, &addressSize);
                fakenet::Packet packet(data, count, transport::SocketAddress(&remoteAddress.gen), s->_ip);
                _inboundPackets.push(packet);
                //
                return;
            }
        }
    }

    void process(const uint64_t timestamp)
    {
        session.processTimeout(timestamp);
        if (!_inboundPackets.empty())
        {
            fakenet::Packet packet;
            if (_inboundPackets.pop(packet))
            {
                for (auto& s : sockets)
                {
                    if (s->_ip == packet.target)
                    {
                        session.onPacketReceived(s.get(), packet.source, packet.data, packet.length, timestamp);
                    }
                }
            }
        }
    }

    jobmanager::JobManager jobManager;
    ice::IceSession session;
    std::unique_ptr<transport::RtcePoll> _socketService;

    std::vector<std::unique_ptr<IceSocketAdapter>> sockets;
    std::atomic_int _socketCount;

    concurrency::MpmcQueue<fakenet::Packet> _inboundPackets;
};

TEST(IceTest, gather)
{
    auto interfaces = transport::SocketAddress::activeInterfaces(false, false);
    ice::IceConfig config;
    IceTestInfra infra(config);
    for (auto interface : interfaces)
    {
        if (interface.getFamily() == AF_INET)
        {
            infra.add(interface);
            break;
        }
    }

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(transport::SocketAddress::parse("64.233.165.127", 19302));
    stunServers.push_back(transport::SocketAddress::parse("216.93.246.18", 3478));
    infra.session.gatherLocalCandidates(stunServers, timeSource);
    for (int i = 0; i < 1000; ++i)
    {
        const auto timeout = infra.session.nextTimeout(timeSource);
        timeSource += timeout + 2;

        infra.process(timeSource);
        if (infra.session.getState() == ice::IceSession::State::READY)
        {
            break;
        }
    }

    {
        auto candidates = infra.session.getLocalCandidates();
        for (auto candidate : candidates)
        {
            logger::info("%s candidate %s:%u",
                "IceTest",
                candidate.type == ice::IceCandidate::Type::HOST ? "host" : "srflx",
                candidate.address.ipToString().c_str(),
                candidate.address.getPort());
        }
    }
    infra.stop();
}

typedef std::vector<std::unique_ptr<ice::IceSession>> IceSessions;
class FakeEndpoint : public ice::IceEndpoint, fakenet::NetworkNode
{
public:
    explicit FakeEndpoint(const transport::SocketAddress& port);
    FakeEndpoint(const transport::SocketAddress& port, fakenet::Gateway& gateway);
    void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& sender,
        const void* data,
        size_t length,
        const uint64_t timestamp) override;
    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;
    transport::SocketAddress getLocalPort() const override { return _address; }
    bool hasIp(const transport::SocketAddress& target) override { return target == _address; }

    void attach(std::unique_ptr<ice::IceSession>& session)
    {
        _session = session.get();
        session->attachLocalEndpoint(this);
    }

    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }
    transport::SocketAddress _address;
    ice::IceSession* _session;

    size_t addressIncompatibilityCount = 0;

private:
    fakenet::Gateway* _gateway;
};

FakeEndpoint::FakeEndpoint(const transport::SocketAddress& port) : _address(port), _session(nullptr), _gateway(nullptr)
{
    assert(!port.empty());
}

FakeEndpoint::FakeEndpoint(const transport::SocketAddress& port, fakenet::Gateway& gateway)
    : _address(port),
      _gateway(&gateway)
{
    assert(!port.empty());
    gateway.addLocal(this);
}

void FakeEndpoint::sendTo(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    const uint64_t timestamp)
{
    if (source.getFamily() != target.getFamily())
    {
        ++addressIncompatibilityCount;
        return;
    }

    if (_address.equalsIp(target))
    {
        logger::debug("received from %s -> %s", "FakeEndpoint", source.toString().c_str(), target.toString().c_str());
        _session->onPacketReceived(this, source, data, length, timestamp);
    }
    else if (_gateway)
    {
        logger::debug("sent %s -> %s", "FakeEndpoint", source.toString().c_str(), target.toString().c_str());
        _gateway->sendTo(source, target, data, length, timestamp);
    }
}

void FakeEndpoint::sendStunTo(const transport::SocketAddress& target,
    __uint128_t transactionId,
    const void* data,
    size_t len,
    const uint64_t timestamp)
{
    sendTo(_address, target, data, len, timestamp);
};

class FakeStunServer : public fakenet::NetworkNode
{
public:
    FakeStunServer(const transport::SocketAddress& port, fakenet::Gateway& internet)
        : _address(port),
          _internet(internet)
    {
        assert(!port.empty());
        internet.addPublic(this);
    }
    void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        const uint64_t timestamp) override
    {
        if (ice::isStunMessage(data, length))
        {
            auto msg = ice::StunMessage::fromPtr(data);

            if (msg->header.getMethod() == ice::StunHeader::BindingRequest)
            {
                ice::StunMessage response;
                response.header.setMethod(ice::StunHeader::BindingResponse);
                response.header.transactionId = msg->header.transactionId;
                response.add(ice::StunXorMappedAddress(source, response.header));
                response.add(ice::StunGenericAttribute(ice::StunAttribute::SOFTWARE, "stunny.org"));

                _internet.sendTo(_address, source, &response, response.size(), timestamp);
            }
        }
    }
    bool hasIp(const transport::SocketAddress& target) override { return target == _address; }

    transport::SocketAddress getIp() const { return _address; }

private:
    const transport::SocketAddress _address;
    fakenet::NetworkNode& _internet;
};

void log(const ice::IceCandidate& candidate, const char* message)
{
    const char* typeName[] = {"host", "srflx", "prflx", "relay"};
    logger::info("%s %s candidate %s %s %s",
        "IceTest",
        message,
        typeName[static_cast<int>(candidate.type)],
        candidate.address.toString().c_str(),
        candidate.baseAddress.empty() ? "" : "base",
        candidate.baseAddress.empty() ? "" : candidate.baseAddress.toString().c_str());
}

void log(const std::vector<ice::IceCandidate>& candidates)
{
    for (auto candidate : candidates)
    {
        log(candidate, "");
    }
}

void gatherCandidates(fakenet::NetworkNode& internet,
    std::vector<transport::SocketAddress>& stunServers,
    IceSessions& sessions,
    uint64_t& timeSource)
{
    const uint64_t startTime = timeSource;
    for (auto& session : sessions)
    {
        session->gatherLocalCandidates(stunServers, timeSource);
    }

    for (bool running = true; running;)
    {
        internet.process(timeSource);
        running = false;
        int64_t timeout = std::numeric_limits<int64_t>::max();
        for (auto& session : sessions)
        {
            auto nextTimeout = session->processTimeout(timeSource);
            if (session->getState() != ice::IceSession::State::READY)
            {
                running = true;
            }
            logger::debug("session timeout is %" PRId64 "ms", "", nextTimeout / utils::Time::ms);
            if (nextTimeout >= 0)
            {
                timeout = std::min(timeout, nextTimeout);
            }
        }
        if (!running || timeout > static_cast<int64_t>(60 * utils::Time::sec))
        {
            break;
        }
        timeSource += timeout + 2;
    }
    logger::info("gather complete in %" PRIu64 "ms", "", (timeSource - startTime) / utils::Time::ms);
}

void exchangeInfo(ice::IceSession& session1, ice::IceSession& session2)
{
    {
        setRemoteCandidates(session2, session1);
        session2.setRemoteCredentials(session1.getLocalCredentials());
    }
    {
        setRemoteCandidates(session1, session2);
        session1.setRemoteCredentials(session2.getLocalCredentials());
    }
}

void exchangeInfo(IceSessions& sessions)
{
    for (size_t i = 0; i < sessions.size(); i += 2)
    {
        exchangeInfo(*sessions[i], *sessions[i + 1]);
    }
}

void logStatus(IceSessions& sessions)
{
    int si = 0;
    for (auto& session : sessions)
    {
        logger::info("session %d local candidates", "", si);
        log(session->getLocalCandidates());
        logger::info("session %d remote candidates", "", si++);
        log(session->getRemoteCandidates());

        if (session->getState() == ice::IceSession::State::CONNECTED)
        {
            logger::info("session %d selected", "", si);
            auto selectedPair1 = session->getSelectedPair();
            log(selectedPair1.first, "local");
            log(selectedPair1.second, "remote");
        }
    }
}

void startProbes(IceSessions& sessions, uint64_t& timeSource)
{
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        logger::info("probing from session %zu", "", i);
        sessions[i]->probeRemoteCandidates(sessions[i]->getRole(), timeSource);
    }
}

bool establishIce(fakenet::NetworkNode& internet, IceSessions& sessions, uint64_t& timeSource, uint64_t runTime)
{
    const auto start = timeSource;
    bool running = true;
    for (running = true; running;)
    {
        internet.process(timeSource);
        int64_t timeout = std::numeric_limits<int64_t>::max();
        running = false;
        for (auto& session : sessions)
        {
            auto sessionTimeout = session->processTimeout(timeSource);
            internet.process(timeSource);
            if (session->getState() == ice::IceSession::State::CONNECTING)
            {
                running = true;
            }
            if (sessionTimeout >= 0)
            {
                timeout = std::min(timeout, sessionTimeout);
            }
        }
        if (running && timeout > 0)
        {
            if (utils::Time::diffGE(start, timeSource + timeout + 2, runTime))
            {
                return false;
            }
            timeSource += timeout + 2;
        }
    }

    return !running;
}

TEST(IceTest, iceprobes)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.0.20", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING, nullptr));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);
    logStatus(sessions);

    auto candidates1 = sessions[0]->getLocalCandidates();
    EXPECT_EQ(candidates1[0].address, endpoint1._address);
    EXPECT_TRUE(candidates1[1].address.equalsIp(firewall1.getPublicIp()));

    auto selectedPair1 = sessions[0]->getSelectedPair();
    EXPECT_FALSE(selectedPair1.first.address.empty());
    EXPECT_FALSE(selectedPair1.second.address.empty());
}

TEST(IceTest, iceprobes2)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000));
    FakeEndpoint endpoint1b(transport::SocketAddress::parse("172.16.2.10", 2001), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.0.20", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING, nullptr));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));

    endpoint1.attach(sessions[0]);
    endpoint1b.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(firewall1.hasIp(pair1.first.address));
    EXPECT_TRUE(pair1.first.baseAddress == endpoint1b._address);
    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);

    EXPECT_TRUE(pair2.first.baseAddress == endpoint2._address);
    EXPECT_TRUE(firewall2.hasIp(pair2.first.address));
}

TEST(IceTest, timerNoCandidates)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000));
    FakeEndpoint endpoint1b(transport::SocketAddress::parse("172.16.2.10", 2001), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.0.20", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING, nullptr));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));

    endpoint1.attach(sessions[0]);
    endpoint1b.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    setRemoteCandidates(*sessions[1], *sessions[0]);
    // session[0] will not have remote candidates
    sessions[1]->setRemoteCredentials(sessions[0]->getLocalCredentials());
    sessions[0]->setRemoteCredentials(sessions[1]->getLocalCredentials());

    sessions[1]->probeRemoteCandidates(ice::IceRole::CONTROLLED, timeSource);
    sessions[0]->probeRemoteCandidates(ice::IceRole::CONTROLLING, timeSource);

    EXPECT_LE(sessions[1]->nextTimeout(timeSource), config.maxRTO * utils::Time::ms);
    EXPECT_EQ(sessions[0]->nextTimeout(timeSource), config.maxRTO * utils::Time::ms);

    auto rc = establishIce(internet, sessions, timeSource, utils::Time::sec * 30);
    EXPECT_TRUE(rc);
}

// client1 behind 2 firewalls. fw2 has private stun server
// fw1 has two public stun servers
// client2 directly on internet
TEST(IceTest, iceblockedroutes)
{
    fakenet::Internet internet;

    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeStunServer stunServer1(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    FakeStunServer stunServer2(transport::SocketAddress::parse("64.233.165.128", 19302), internet);
    // stun3 only reachable from firewall2
    FakeStunServer stunServer3(transport::SocketAddress::parse("64.233.165.129", 19302), firewall2);

    FakeEndpoint endpoint1a(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint1b(transport::SocketAddress::parse("172.16.0.11", 2001), firewall1);
    FakeEndpoint endpoint1c(transport::SocketAddress::parse("172.16.0.12", 2001), firewall2);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("192.16.2.20", 3000), internet);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1a.attach(sessions[0]);
    endpoint1b.attach(sessions[0]);
    endpoint1c.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer1.getIp());
    stunServers.push_back(stunServer2.getIp());
    stunServers.push_back(stunServer3.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();

    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);
    EXPECT_TRUE(pair2.first.baseAddress == endpoint2._address);
}

TEST(IceTest, fixedportmap)
{
    fakenet::Internet internet;

    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeStunServer stunServer1(transport::SocketAddress::parse("64.233.165.127", 19302), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.2.20", 3000), firewall2);

    // static port map to firewall public interface
    firewall2.addPortMapping(endpoint2._address, endpoint2._address.getPort());

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer1.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    sessions[0]->gatherLocalCandidates(stunServers, timeSource);

    while (sessions[0]->getState() != ice::IceSession::State::READY)
    {
        internet.process(timeSource);
        sessions[0]->processTimeout(timeSource);
        internet.process(timeSource);
        if (sessions[0]->getState() != ice::IceSession::State::READY)
        {
            timeSource += sessions[0]->nextTimeout(timeSource) + 2;
        }
    }
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(firewall1.hasIp(pair1.first.address));

    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);
}

TEST(IceTest, noroute)
{
    fakenet::Internet internet;

    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.2.20", 3000), firewall2);

    // static port map to firewall public interface
    firewall2.addPortMapping(endpoint2._address, endpoint2._address.getPort());

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(pair1.first.empty());
    EXPECT_TRUE(pair2.second.empty());
}

TEST(IceTest, fixedportmapNogathering)
{
    fakenet::Internet internet;

    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.2.20", 3000), firewall2);

    // static port map to firewall public interface
    firewall2.addPortMapping(endpoint2._address, endpoint2._address.getPort());

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    sessions[0]->addRemoteCandidate(ice::IceCandidate("910823",
        ice::IceComponent::RTP,
        ice::TransportType::UDP,
        5001,
        transport::SocketAddress(firewall2.getPublicIp(), endpoint2._address.getPort()),
        endpoint2._address,
        ice::IceCandidate::Type::SRFLX));

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(firewall1.hasIp(pair1.first.address));

    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);
}

TEST(IceTest, icev6)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("a000:1092:10cc:f56e::3cb4", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("a000:1092:10cc:f56e::3c00", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("a000:1092:10cc:f56e::3c01", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1000", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1001", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(firewall1.hasIp(pair1.first.address));

    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);
}

TEST(IceTest, icev6sameFw)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("a000:1092:10cc:f56e::3cb4", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("a000:1092:10cc:f56e::3c00", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1000", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1001", 3000), firewall1);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(endpoint1.hasIp(pair1.first.address));
    EXPECT_TRUE(endpoint2.hasIp(pair2.first.address));
    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);
}

TEST(IceTest, icev6v4Mix)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("a000:1092:10cc:f56e::3cb4", 19302), internet);
    FakeStunServer stunServerIp4(transport::SocketAddress::parse("217.0.10.15", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("a000:1092:10cc:f56e::3c00", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("a000:1092:10cc:f56e::3c01", 0), internet);
    fakenet::Firewall firewall3(transport::SocketAddress::parse("217.0.10.10", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1000", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("2001:7ed8:0fce:3d87::1001", 3000), firewall2);
    FakeEndpoint endpoint3(transport::SocketAddress::parse("192.168.0.11", 3000), firewall3);
    FakeEndpoint endpoint4(transport::SocketAddress::parse("10.10.11.14", 3000), firewall3);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);
    endpoint3.attach(sessions[0]);
    endpoint4.attach(sessions[0]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());
    stunServers.push_back(stunServerIp4.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    exchangeInfo(sessions);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    auto pair1 = sessions[0]->getSelectedPair();
    auto pair2 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(firewall1.hasIp(pair1.first.address));

    EXPECT_TRUE(pair2.first.address == pair1.second.address);
    EXPECT_TRUE(pair1.first.address == pair2.second.address);

    EXPECT_EQ(endpoint1.addressIncompatibilityCount, 0);
    EXPECT_EQ(endpoint2.addressIncompatibilityCount, 0);
    EXPECT_EQ(endpoint3.addressIncompatibilityCount, 0);
    EXPECT_EQ(endpoint4.addressIncompatibilityCount, 0);
}

TEST(IceRobustness, badLength)
{
    using namespace ice;
    StunMessage msg;
    msg.add(StunGenericAttribute(StunAttribute::SOFTWARE, "test"));
    msg.add(StunGenericAttribute(StunAttribute::SOFTWARE, "test1"));
    auto attrIt = msg.begin();
    attrIt->length = msg.size(); // corrupt the size to SEGF

    EXPECT_FALSE(isStunMessage(&msg, msg.size()));
    EXPECT_FALSE(msg.isValid());
}

TEST(IceRobustness, badAddress)
{
    using namespace ice;
    StunMessage msg;
    msg.add(StunXorMappedAddress(transport::SocketAddress::parse("128.0.4.1", 90), msg.header));
    auto attrIt = msg.begin();
    attrIt->length = attrIt->size() + 3; // corrupt the size to SEGF

    msg.add(StunGenericAttribute(StunAttribute::SOFTWARE, "test"));

    EXPECT_TRUE(isStunMessage(&msg, msg.size()));
    EXPECT_FALSE(msg.isValid());
}

TEST(IceRobustness, stringNullTermination)
{
    using namespace ice;
    StunMessage msg;
    msg.add(StunGenericAttribute(StunAttribute::SOFTWARE, "corrupt"));
    auto attrIt = msg.begin();
    char* d = reinterpret_cast<char*>(&(attrIt->length) + 1);
    d[7] = 'm';
    EXPECT_EQ("corrupt", reinterpret_cast<StunGenericAttribute&>(*attrIt).getUtf8());
    EXPECT_TRUE(isStunMessage(&msg, msg.size()));
    EXPECT_TRUE(msg.isValid());

    d[6] = 0;
    EXPECT_EQ("corrup", reinterpret_cast<StunGenericAttribute&>(*attrIt).getUtf8());
    EXPECT_TRUE(isStunMessage(&msg, msg.size()));
    EXPECT_TRUE(msg.isValid());
}

TEST(IceRobustness, earlyProbes)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.0.20", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLING, nullptr));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    // provide one side with candidates and credentials
    logger::info("GATHER PHASE COMPLETE", "");
    setRemoteCandidates(*sessions[1], *sessions[0]);
    sessions[1]->setRemoteCredentials(sessions[0]->getLocalCredentials());

    int si = 0;
    for (auto& session : sessions)
    {
        logger::info("session %d local candidates", "", si);
        log(session->getLocalCandidates());
        logger::info("remote candidates", "");
        log(session->getRemoteCandidates());
    }

    EXPECT_EQ(sessions[0]->getRemoteCandidates().size(), 0);

    logger::info("probing from session %u", "", 0);
    sessions[1]->probeRemoteCandidates(ice::IceRole::CONTROLLING, timeSource);

    int iterations = 0;
    const auto startTimeNoCredentials = timeSource;
    for (bool running = true; running && timeSource - startTimeNoCredentials < utils::Time::sec * 5; iterations++)
    {
        internet.process(timeSource);
        int64_t timeout = std::numeric_limits<int64_t>::max();
        running = false;
        for (auto& session : sessions)
        {
            auto sessionTimeout = session->processTimeout(timeSource);
            internet.process(timeSource);
            if (session->getState() == ice::IceSession::State::CONNECTING)
            {
                running = true;
            }
            if (sessionTimeout >= 0)
            {
                timeout = std::min(timeout, sessionTimeout);
            }
        }
        if (running && timeout > 0)
        {
            timeSource += timeout + 2;
        }
    }

    ASSERT_EQ(sessions[0]->getRemoteCandidates().size(), 1);
    EXPECT_EQ(sessions[0]->getState(), ice::IceSession::State::READY);

    const auto session0Remotes = sessions[0]->getRemoteCandidates();
    EXPECT_EQ(session0Remotes[0].type, ice::IceCandidate::Type::PRFLX);
    EXPECT_TRUE(session0Remotes[0].address.equalsIp(firewall2.getPublicIp()));
    // session 1 has a candidate, but session 0 cannot send request back on the probe

    sessions[0]->setRemoteCredentials(sessions[1]->getLocalCredentials());
    setRemoteCandidates(*sessions[0], *sessions[1]);
    sessions[0]->probeRemoteCandidates(sessions[1]->getRole(), timeSource);
    for (bool running = true; running; iterations++)
    {
        internet.process(timeSource);
        int64_t timeout = std::numeric_limits<int64_t>::max();
        running = false;
        for (auto& session : sessions)
        {
            auto sessionTimeout = session->processTimeout(timeSource);
            internet.process(timeSource);
            if (session->getState() == ice::IceSession::State::CONNECTING)
            {
                running = true;
            }
            if (sessionTimeout >= 0)
            {
                timeout = std::min(timeout, sessionTimeout);
            }
        }
        if (running && timeout > 0)
        {
            timeSource += timeout + 2;
        }
    }

    EXPECT_EQ(sessions[0]->getRemoteCandidates().size(), 2);
    EXPECT_GE(sessions[0]->getState(), ice::IceSession::State::CONNECTING);
    EXPECT_LE(sessions[0]->getState(), ice::IceSession::State::CONNECTED);

    for (auto& session : sessions)
    {
        logger::info("session state %d", "", session->getState());
        log(session->getLocalCandidates());
        log(session->getRemoteCandidates());

        logger::info("selected", "");
        auto selectedPair1 = session->getSelectedPair();
        log(selectedPair1.first, "local");
        log(selectedPair1.second, "remote");
    }

    const auto duration = timeSource - startTimeNoCredentials;
    EXPECT_LT(duration, utils::Time::sec * 6);
    auto candidates1 = sessions[0]->getLocalCandidates();
    ASSERT_EQ(candidates1.size(), 2);
    EXPECT_EQ(candidates1[0].address, endpoint1._address);
    EXPECT_TRUE(candidates1[1].address.equalsIp(firewall1.getPublicIp()));

    auto selectedPair0 = sessions[0]->getSelectedPair();
    EXPECT_EQ(selectedPair0.first.baseAddress, endpoint1._address);
    EXPECT_TRUE(selectedPair0.first.address.equalsIp(firewall1.getPublicIp()));
    EXPECT_EQ(selectedPair0.first.type, ice::IceCandidate::Type::PRFLX);
    EXPECT_TRUE(selectedPair0.second.address.equalsIp(firewall2.getPublicIp()));

    auto remoteCandidates1 = sessions[0]->getRemoteCandidates();
    EXPECT_EQ(remoteCandidates1.size(), 2);
    auto selectedPair1 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(selectedPair1.second.address.equalsIp(firewall1.getPublicIp()));
}

TEST(IceRobustness, roleConflict)
{
    fakenet::Internet internet;

    FakeStunServer stunServer(transport::SocketAddress::parse("64.233.165.127", 19302), internet);
    fakenet::Firewall firewall1(transport::SocketAddress::parse("216.93.246.10", 0), internet);
    fakenet::Firewall firewall2(transport::SocketAddress::parse("216.93.24.11", 0), internet);

    FakeEndpoint endpoint1(transport::SocketAddress::parse("172.16.0.10", 2000), firewall1);
    FakeEndpoint endpoint2(transport::SocketAddress::parse("172.16.0.20", 3000), firewall2);

    ice::IceConfig config;
    IceSessions sessions;
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(1, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));
    sessions.emplace_back(
        std::make_unique<ice::IceSession>(2, config, ice::IceComponent::RTP, ice::IceRole::CONTROLLED, nullptr));

    endpoint1.attach(sessions[0]);
    endpoint2.attach(sessions[1]);

    std::vector<transport::SocketAddress> stunServers;
    stunServers.push_back(stunServer.getIp());

    uint64_t timeSource = utils::Time::getAbsoluteTime();
    gatherCandidates(internet, stunServers, sessions, timeSource);
    // provide one side with candidates and credentials
    logger::info("GATHER PHASE COMPLETE", "");
    exchangeInfo(*sessions[0], *sessions[1]);
    startProbes(sessions, timeSource);
    establishIce(internet, sessions, timeSource, utils::Time::sec * 30);

    ASSERT_EQ(sessions[0]->getState(), ice::IceSession::State::CONNECTED);
    ASSERT_EQ(sessions[1]->getState(), ice::IceSession::State::CONNECTED);
    auto selectedPair0 = sessions[0]->getSelectedPair();
    EXPECT_TRUE(selectedPair0.second.address.equalsIp(firewall2.getPublicIp()));

    auto selectedPair1 = sessions[1]->getSelectedPair();
    EXPECT_TRUE(selectedPair1.second.address.equalsIp(firewall1.getPublicIp()));
    if (sessions[0]->getRole() == ice::IceRole::CONTROLLING)
    {
        EXPECT_EQ(sessions[1]->getRole(), ice::IceRole::CONTROLLED);
    }
    else
    {
        EXPECT_EQ(sessions[1]->getRole(), ice::IceRole::CONTROLLING);
    }
}
