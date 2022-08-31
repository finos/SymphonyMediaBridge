#include "transport/RtcSocket.h"
#include "utils/Time.h"
#include <array>
#include <gtest/gtest.h>
#ifdef __APPLE__
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len; // number of received bytes for header
};
#endif
struct ReceivedMessage
{
    transport::RawSockAddress src_addr;
    iovec iobuffer;
    char packet[1500];

    bool link(mmsghdr& header)
    {
        iobuffer.iov_base = packet;
        iobuffer.iov_len = 1500;

        header.msg_hdr.msg_control = nullptr;
        header.msg_hdr.msg_controllen = 0;
        header.msg_hdr.msg_flags = MSG_DONTWAIT;
        header.msg_hdr.msg_iov = &iobuffer;
        header.msg_hdr.msg_iovlen = 1;
        header.msg_hdr.msg_name = &src_addr;
        header.msg_hdr.msg_namelen = sizeof(src_addr);

        header.msg_len = 0;

        return true;
    }
};

struct Ipv6Test : public ::testing::Test
{
    transport::RtcSocket sender;
    transport::RtcSocket receiver;
    transport::SocketAddress badTarget;
    transport::SocketAddress goodTarget;
    transport::SocketAddress ipv6Target;

    void SetUp()
    {
        sender.open(transport::SocketAddress::parse("127.0.0.1"), 11003);
        receiver.open(transport::SocketAddress::parse("127.0.0.1"), 11004);
        badTarget = transport::SocketAddress::parse("0.0.0.128", 11000);
        goodTarget = transport::SocketAddress::parse("127.0.0.1", 11004);
        ipv6Target = transport::SocketAddress::parse("2a01:4c8:61:6471:1:2:7ba0:8949", 11002);
    }

    void TearDown()
    {
        sender.close();
        receiver.close();
    }
};

TEST_F(Ipv6Test, send1stBad)
{
    using namespace transport;

    std::array<RtcSocket::Message, 10> packets;
    const char* data = "This is a test message of no concern.";

    packets[0].add(data, std::strlen(data));
    packets[0].target = &badTarget;
    packets[1].add(data, std::strlen(data));
    packets[1].target = &goodTarget;

    int rcSend = sender.sendMultiple(packets.data(), 2);
    EXPECT_EQ(rcSend, 1);
    utils::Time::uSleep(50000);

    mmsghdr messageHeader[2];
    ReceivedMessage recvMessages[2];
    recvMessages[0].link(messageHeader[0]);
    recvMessages[1].link(messageHeader[1]);

    const int flags = MSG_DONTWAIT;
    ssize_t byteCount = ::recvmsg(receiver.fd(), &messageHeader[0].msg_hdr, flags);
    ssize_t byteCount2 = ::recvmsg(receiver.fd(), &messageHeader[1].msg_hdr, flags);

    EXPECT_EQ(byteCount, packets[1].getLength());
    EXPECT_EQ(byteCount2, -1);
#ifdef __APPLE__
    EXPECT_EQ(packets[0].errorCode, EHOSTUNREACH);
#else
    EXPECT_EQ(packets[0].errorCode, EINVAL);
#endif
}

TEST_F(Ipv6Test, send2ndBad)
{
    using namespace transport;

    std::array<RtcSocket::Message, 10> packets;
    const char* data = "This is a test message of no concern.";

    packets[0].add(data, std::strlen(data));
    packets[0].target = &goodTarget;
    packets[1].add(data, std::strlen(data));
    packets[1].target = &badTarget;
    packets[2].add(data, std::strlen(data));
    packets[2].target = &goodTarget;

    int rcSend = sender.sendMultiple(packets.data(), 3);
    EXPECT_EQ(rcSend, 1);
    utils::Time::uSleep(50000);

    mmsghdr messageHeader[2];
    ReceivedMessage recvMessages[2];
    recvMessages[0].link(messageHeader[0]);
    recvMessages[1].link(messageHeader[1]);

    const int flags = MSG_DONTWAIT;
    ssize_t byteCount = ::recvmsg(receiver.fd(), &messageHeader[0].msg_hdr, flags);
    ssize_t byteCount2 = ::recvmsg(receiver.fd(), &messageHeader[1].msg_hdr, flags);

    EXPECT_EQ(byteCount, packets[1].getLength());
    EXPECT_EQ(byteCount2, packets[1].getLength());
#ifdef __APPLE__
    EXPECT_EQ(packets[1].errorCode, EHOSTUNREACH);
#else
    EXPECT_EQ(packets[1].errorCode, EINVAL);
#endif
}

TEST_F(Ipv6Test, sendLastBad)
{
    using namespace transport;

    std::array<RtcSocket::Message, 10> packets;
    const char* data = "This is a test message of no concern.";

    packets[0].add(data, std::strlen(data));
    packets[0].target = &goodTarget;
    packets[1].add(data, std::strlen(data));
    packets[1].target = &goodTarget;
    packets[2].add(data, std::strlen(data));
    packets[2].target = &badTarget;
    packets[3].add(data, std::strlen(data));
    packets[3].target = &ipv6Target;

    int rcSend = sender.sendMultiple(packets.data(), 4);
    EXPECT_EQ(rcSend, 2);

    utils::Time::uSleep(50000);

    mmsghdr messageHeader[2];
    ReceivedMessage recvMessages[2];
    recvMessages[0].link(messageHeader[0]);
    recvMessages[1].link(messageHeader[1]);

    const int flags = MSG_DONTWAIT;
    ssize_t byteCount = ::recvmsg(receiver.fd(), &messageHeader[0].msg_hdr, flags);
    ssize_t byteCount2 = ::recvmsg(receiver.fd(), &messageHeader[1].msg_hdr, flags);
    EXPECT_EQ(byteCount, packets[1].getLength());
    EXPECT_EQ(byteCount2, packets[1].getLength());
#ifdef __APPLE__
    EXPECT_EQ(packets[3].errorCode, EHOSTUNREACH);
    EXPECT_EQ(packets[2].errorCode, EHOSTUNREACH);
#else
    EXPECT_EQ(packets[3].errorCode, EAFNOSUPPORT);
    EXPECT_EQ(packets[2].errorCode, EINVAL);
#endif
}
