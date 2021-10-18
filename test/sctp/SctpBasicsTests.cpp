
#include "SctpEndpoint.h"
#include "concurrency/MpmcQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "test/transport/FakeNetwork.h"
#include "transport/sctp/SctpAssociation.h"
#include "transport/sctp/SctpConfig.h"
#include "transport/sctp/SctpServerPort.h"
#include "transport/sctp/Sctprotocol.h"
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
using namespace transport;
using namespace testing;
using namespace sctptest;

namespace
{

} // namespace
struct SctpTestFixture : public ::testing::Test
{
    SctpTestFixture() : _timestamp(utils::Time::getAbsoluteTime()) {}

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

TEST_F(SctpTestFixture, setup)
{
    SctpEndpoint A(5000, _config, _timestamp);
    SctpEndpoint B(5001, _config, _timestamp);

    _timestamp += 10 * utils::Time::ms;
    A.connect(5001);

    for (int i = 0; i < 20; ++i)
    {
        _timestamp += 3 * utils::Time::ms;
        A.forwardPacket(B);
        B.forwardPacket(A);
    }
    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
    EXPECT_EQ(B._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
}

TEST_F(SctpTestFixture, InitInCookieEchoed)
{
    SctpEndpoint A(5000, _config, _timestamp, 100000);
    SctpEndpoint B(5001, _config, _timestamp, 100000);

    _timestamp += 10 * utils::Time::ms;
    A.connect(5001);
    _timestamp += 5 * utils::Time::ms;
    A.forwardPacket(B); // init to ep2
    B.forwardPacket(A); // init_ack to ep1
    B.connect(5000);
    _timestamp += 5 * utils::Time::ms;
    B.forwardPacket(A); // init to ep1
    _timestamp += 5 * utils::Time::ms;
    A.forwardPacket(B); // cookie echo to ep2
    _timestamp += 5 * utils::Time::ms;
    A.forwardPacket(B); // init again to ep2
    B.forwardPacket(A);

    _timestamp += 5 * utils::Time::ms;
    A.forwardPacket(B);
    _timestamp += 5 * utils::Time::ms;
    B.forwardPacket(A);

    _timestamp += 5 * utils::Time::ms;
    A.forwardPacket(B);
    _timestamp += 5 * utils::Time::ms;
    B.forwardPacket(A);

    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
    EXPECT_EQ(B._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
}

TEST_F(SctpTestFixture, InitbeforeINIT_ACK)
{
    SctpEndpoint A(5000, _config, _timestamp, 100000);
    SctpEndpoint B(5001, _config, _timestamp, 100000);

    _timestamp += 10 * utils::Time::ms;
    A.connect(5001);
    _timestamp += 1 * utils::Time::ms;
    A.forwardPacket(B); // init to B, will start stateless cookie
    B.connect(5000);
    _timestamp += 1 * utils::Time::ms;
    B.forwardPacket(A); // B' INIT to A
    _timestamp += 1 * utils::Time::ms;

    A.forwardPacket(B); // init_ack to B'
    _timestamp += 1 * utils::Time::ms;
    B.forwardPacket(A); // B INIT_ACK to A

    _timestamp += 1 * utils::Time::ms;
    A.forwardPacket(B); // COOKIE_ECHO
    _timestamp += 1 * utils::Time::ms;
    B.forwardPacket(A); // COOKIE_ECHO
    _timestamp += 1 * utils::Time::ms;
    A.forwardPacket(B);
    _timestamp += 1 * utils::Time::ms;

    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
    EXPECT_EQ(B._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
    A.forwardPacket(B); // empty
    _timestamp += 1 * utils::Time::ms;
    B.forwardPacket(A); // empty
    _timestamp += 1 * utils::Time::ms;
    A.forwardPacket(B); // empty
    _timestamp += 1 * utils::Time::ms;
    B.forwardPacket(A); // empty

    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
    EXPECT_EQ(B._session->getState(), sctp::SctpAssociation::State::ESTABLISHED);
}

TEST_F(SctpTestFixture, InitTimeout)
{
    SctpEndpoint A(5000, _config, _timestamp, 500000);
    SctpEndpoint blackHole(5001, _config, _timestamp, 500000);

    _timestamp += 10 * utils::Time::ms;
    A.connect(5001);
    EXPECT_EQ(A._sendQueue.count(), 1);
    A.forwardWhenReady(blackHole);
    _timestamp += (_config.init.timeout - 5) * utils::Time::ms;
    auto toSleep = A.process();
    EXPECT_EQ(A._sendQueue.count(), 0);
    EXPECT_GE(toSleep, 3 * utils::Time::ms);
    _timestamp += toSleep + 1;
    toSleep = A.process();
    EXPECT_EQ(A._sendQueue.count(), 1);
    _timestamp += 1 * utils::Time::ms;
    A.forwardWhenReady(blackHole);
    for (int i = 0; i < _config.init.maxRetransmits - 1; ++i)
    {
        _timestamp += toSleep + 1;
        toSleep = A.process();
        EXPECT_EQ(A._sendQueue.count(), i + 1);
        EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::COOKIE_WAIT);
    }

    _timestamp += toSleep + 1;
    A.process();
    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::CLOSED);
}

TEST_F(SctpTestFixture, CookieTimeout)
{
    SctpEndpoint A(5000, _config, _timestamp, 500000);
    SctpEndpoint B(5001, _config, _timestamp, 500000);
    SctpEndpoint blackHole(6001, _config, _timestamp);

    _timestamp += 10 * utils::Time::ms;
    A.connect(5001);

    A.forwardWhenReady(B); // INIT
    EXPECT_EQ(B._session, nullptr);
    B.forwardWhenReady(A); // INIT_ACK
    uint64_t cookieSendTime = _timestamp;
    EXPECT_EQ(A._sendQueue.count(), 1); // should be a cookie there

    A.forwardWhenReady(blackHole); // drop cookie echo
    auto duration = _timestamp - cookieSendTime;
    _timestamp += (_config.init.timeout - 2) * utils::Time::ms - duration;
    auto toSleep = A.process();
    EXPECT_EQ(A._sendQueue.count(), 0);
    EXPECT_GE(toSleep, 1 * utils::Time::ms);
    _timestamp += toSleep + 1;
    toSleep = A.process();
    EXPECT_EQ(A._sendQueue.count(), 1);

    A.forwardWhenReady(blackHole);
    EXPECT_EQ(B._session, nullptr);
    for (int i = 0; i < _config.init.maxRetransmits - 1; ++i)
    {
        _timestamp += toSleep + 1;
        toSleep = A.process();
        EXPECT_EQ(A._sendQueue.count(), i + 1);
        EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::COOKIE_ECHOED);
    }

    _timestamp += toSleep + 1;
    A.process();
    EXPECT_EQ(A._session->getState(), sctp::SctpAssociation::State::CLOSED);
}