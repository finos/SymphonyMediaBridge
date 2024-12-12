#include "api/utils.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/dtls/DtlsMessageListener.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SrtpProfiles.h"
#include "transport/dtls/SslDtls.h"
#include "transport/dtls/SslWriteBioListener.h"
#include "utils/Time.h"
#include <cassert>
#include <gtest/gtest.h>
#include <memory>

using namespace testing;

struct FakeSrtpEndpoint : public transport::SslWriteBioListener
{
    FakeSrtpEndpoint(transport::SrtpClient& client,
        transport::DtlsMessageListener& peer,
        memory::PacketPoolAllocator& allocator)
        : _dtlsPackets(256),
          _peer(peer),
          _allocator(allocator)
    {
        client.setSslWriteBioListener(this);
    }

    ~FakeSrtpEndpoint() { _dtlsPackets.clear(); }

    int32_t sendDtls(const char* buffer, uint32_t length) override
    {
        if (transport::isDtlsPacket(buffer, length))
        {
            _dtlsPackets.push(memory::makeUniquePacket(_allocator, buffer, length));
            return length;
        }
        else
        {
            // rtp
        }

        return length;
    }

    void process()
    {
        for (memory::UniquePacket packet; _dtlsPackets.pop(packet);)
        {
            _peer.onMessageReceived(std::move(packet));
        }
    }

    concurrency::MpmcQueue<memory::UniquePacket> _dtlsPackets;
    transport::DtlsMessageListener& _peer;
    memory::PacketPoolAllocator& _allocator;
};

struct SrtpTest : public ::testing::Test, public transport::SrtpClient::IEvents
{
    SrtpTest() : _allocator(4096, "srtpAllocator") {}

    void SetUp() override
    {
        _dtls = std::make_unique<transport::SslDtls>();
        assert(_dtls->isInitialized());
        _factory = std::make_unique<transport::SrtpClientFactory>(*_dtls);

        _srtp1 = _factory->create(this);
        _srtp2 = _factory->create(this);
        _ep1 = std::make_unique<FakeSrtpEndpoint>(*_srtp1, *_srtp2, _allocator);
        _ep2 = std::make_unique<FakeSrtpEndpoint>(*_srtp2, *_srtp1, _allocator);

        auto header = rtp::RtpHeader::create(_audioPacket);
        auto payload = header->getPayload();
        for (size_t i = 0; i < _audioPacket.getLength() - header->headerLength(); ++i)
        {
            payload[i] = i;
        }
    }

    void setupDtls()
    {
        _srtp1->setRemoteDtlsFingerprint("sha-256", _dtls->getLocalFingerprint(), true);
        _srtp2->setRemoteDtlsFingerprint("sha-256", _dtls->getLocalFingerprint(), false);
    }

    void setupSdes(srtp::Profile profile)
    {
        srtp::AesKey key1;
        srtp::AesKey key2;
        _srtp1->getLocalKey(profile, key1);
        _srtp2->getLocalKey(profile, key2);
        _srtp1->setRemoteKey(key2);
        _srtp2->setRemoteKey(key1);
    }

    void connect()
    {
        for (int i = 0; i < 500 && !(_srtp1->isConnected() && _srtp2->isConnected()); ++i)
        {
            _srtp1->processTimeout();
            _srtp2->processTimeout();
            _ep1->process();
            _ep2->process();
            utils::Time::nanoSleep(10 * utils::Time::ms);
        }
    }
    void onSrtpStateChange(transport::SrtpClient* srtpClient, transport::SrtpClient::State state) override
    {
        logger::info("SRTP state change %s", srtpClient->getLoggableId().c_str(), api::utils::toString(state));
    }

    bool isDataValid(uint8_t* data) const
    {
        auto header = rtp::RtpHeader::fromPacket(_audioPacket);
        auto payload = header->getPayload();
        for (size_t i = 0; i < _audioPacket.getLength() - header->headerLength(); ++i)
        {
            if (payload[i] != data[i])
            {
                return false;
            }
        }
        return true;
    }

    bool isAudioPayloadValid(memory::Packet& newPacket) const
    {
        auto header = rtp::RtpHeader::fromPacket(_audioPacket);
        auto payload = header->getPayload();

        auto newPayload = rtp::RtpHeader::fromPacket(newPacket)->getPayload();
        for (size_t i = 0; i < _audioPacket.getLength() - header->headerLength(); ++i)
        {
            if (payload[i] != newPayload[i])
            {
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<transport::SrtpClientFactory> _factory;
    std::unique_ptr<transport::SslDtls> _dtls;
    memory::PacketPoolAllocator _allocator;
    std::unique_ptr<transport::SrtpClient> _srtp1; // = _factory->create(this);
    std::unique_ptr<transport::SrtpClient> _srtp2; //    auto srtp2 = _factory->create(this);
    std::unique_ptr<FakeSrtpEndpoint> _ep1;
    std::unique_ptr<FakeSrtpEndpoint> _ep2;
    memory::Packet _audioPacket;
};

TEST_F(SrtpTest, seqSkip)
{
    setupDtls();
    connect();

    uint16_t seqStart = 65530;
    for (int i = 0; i < 65535 * 4; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->timestamp = i * 160;

        if (i == 90000)
        {
            i += 35000;
            EXPECT_TRUE(_srtp1->setLocalRolloverCounter(1, 1 + (i >> 16)));
        }
        if (i == 90000 + 35000 + 540)
        {
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, 1 + (i >> 16)));
        }

        header->sequenceNumber = seqStart + i;

        EXPECT_TRUE(_srtp1->protect(*packet));
        if (i >= 90000 + 35000 && i < 90000 + 35000 + 540)
        {
            EXPECT_FALSE(_srtp2->unprotect(*packet));
        }
        else
        {
            EXPECT_TRUE(_srtp2->unprotect(*packet));

            auto header = rtp::RtpHeader::fromPacket(*packet);
            EXPECT_TRUE(isDataValid(header->getPayload()));
            if ((i % 1000) == 0)
            {
                logger::debug("encrypted %u packets", "SrtpTest", i + 1);
            }
        }
    }
}

TEST_F(SrtpTest, seqDuplicate)
{
    setupDtls();
    connect();

    auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
    auto header = rtp::RtpHeader::fromPacket(*packet);
    header->ssrc = 4321;
    header->timestamp = 1234;
    header->sequenceNumber = 5678;

    EXPECT_TRUE(_srtp1->protect(*packet));

    auto packetCopy = memory::makeUniquePacket(_allocator, *packet);

    EXPECT_TRUE(_srtp2->unprotect(*packet));
    EXPECT_FALSE(_srtp2->unprotect(*packetCopy));
}

TEST_F(SrtpTest, sendOutOfOrder)
{
    setupDtls();
    connect();

    for (int i = 0; i < 50; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = 5678 + i;

        EXPECT_TRUE(_srtp1->protect(*packet));

        EXPECT_TRUE(_srtp2->unprotect(*packet));
    }

    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = 5678 + 55;

        EXPECT_TRUE(_srtp1->protect(*packet));

        EXPECT_TRUE(_srtp2->unprotect(*packet));
    }

    for (int i = 51; i < 55; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = 5678 + i;

        EXPECT_TRUE(_srtp1->protect(*packet));

        EXPECT_TRUE(_srtp2->unprotect(*packet));
    }
}

TEST_F(SrtpTest, sdesSimple)
{
    setupSdes(srtp::Profile::AES128_CM_SHA1_80);

    EXPECT_TRUE(_srtp1->isConnected());
    EXPECT_TRUE(_srtp2->isConnected());
    auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
    auto header = rtp::RtpHeader::fromPacket(*packet);
    header->ssrc = 4321;
    header->timestamp = 1234;
    header->sequenceNumber = 5678;

    size_t dataLen = packet->getLength();
    EXPECT_TRUE(_srtp1->protect(*packet));
    EXPECT_FALSE(isAudioPayloadValid(*packet));
    EXPECT_GT(packet->getLength(), dataLen);
    EXPECT_TRUE(_srtp2->unprotect(*packet));
    EXPECT_EQ(dataLen, packet->getLength());
    EXPECT_TRUE(isAudioPayloadValid(*packet));
}

TEST_F(SrtpTest, sendReplayWindow)
{
    setupDtls();
    connect();

    int seq = 5678;
    logger::debug("encrypt %u", "", seq);
    // fill lib SRTP seqno cache
    for (int i = 0; i < 32 * 1024; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = seq++;

        EXPECT_TRUE(_srtp1->protect(*packet));

        EXPECT_TRUE(_srtp2->unprotect(*packet));
    }
    logger::debug("encrypted %u", "", seq - 1);

    // right before the replay window
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = seq + 1024 * 31;
        logger::debug("encrypt %u", "", header->sequenceNumber.get());
        EXPECT_TRUE(_srtp1->protect(*packet));
    }

    // wrap into the replay window
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 4321;
        header->timestamp = 1234;
        header->sequenceNumber = seq + 1024 * 31;
        logger::debug("encrypt %u", "", header->sequenceNumber.get());
        EXPECT_FALSE(_srtp1->protect(*packet));
    }
}

TEST_F(SrtpTest, sendRoc)
{
    setupDtls();
    connect();

    uint16_t seqStart = 5;
    uint32_t prevRoc = 0;

    for (uint32_t i = seqStart; i < 170000; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;

        ASSERT_TRUE(_srtp1->protect(*packet));

        if (prevRoc != (i >> 16))
        {
            prevRoc = i >> 16;
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, prevRoc)); // works without this due to continuous sequence
        }
        ASSERT_TRUE(_srtp2->unprotect(*packet));
    }
}

TEST_F(SrtpTest, sendRocGap)
{
    setupDtls();
    connect();

    uint16_t seqStart = 5;
    uint32_t prevRoc = 0;

    for (uint32_t i = seqStart; i < 200000; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;

        ASSERT_TRUE(_srtp1->protect(*packet));

        if (i > 64000 && i < 66000)
        {
            continue;
        }
        if (i > 130000 && i < 190000)
        {
            continue;
        }
        if (prevRoc != (i >> 16))
        {
            prevRoc = i >> 16;
            logger::info("set roc %u", "", prevRoc);
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, prevRoc)); // works without this due to continuous sequence
        }
        ASSERT_TRUE(_srtp2->unprotect(*packet));
    }
}

TEST_F(SrtpTest, rocReorder)
{
    setupDtls();
    connect();

    uint32_t prevRoc = 0;
    uint32_t prevSendRoc = 0;

    std::vector<uint32_t> seqNos(0);
    seqNos.reserve(32000);
    for (uint32_t s = 35000; s < 67000; ++s)
    {
        seqNos.push_back(s);
    }

    for (size_t i = 0; i < seqNos.size() - 10; ++i)
    {
        if (rand() % 1000 < 300)
        {
            std::swap(seqNos[i], seqNos[i + (rand() % 10)]);
        }
    }

    for (uint32_t i = 0; i < seqNos.size(); ++i)
    {
        const uint32_t extSeqNo = seqNos[i];
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::create(*packet);
        header->ssrc = 1;
        header->sequenceNumber = extSeqNo & 0xFFFFu;
        header->timestamp = i * 160;

        if (prevSendRoc != extSeqNo >> 16)
        {
            EXPECT_TRUE(_srtp1->setLocalRolloverCounter(1, extSeqNo >> 16));
            prevSendRoc = extSeqNo >> 16;
        }
        ASSERT_TRUE(_srtp1->protect(*packet));

        if (prevRoc != (extSeqNo >> 16))
        {
            prevRoc = extSeqNo >> 16;
            logger::info("set roc %u", "", prevRoc);
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, prevRoc));
            // sequence
        }
        ASSERT_TRUE(_srtp2->unprotect(*packet));
    }
}

TEST_F(SrtpTest, receiveRocGap)
{
    setupDtls();
    connect();

    uint16_t seqStart = 50000;
    uint32_t unprotectedExtSeqNo = 0;

    for (uint32_t i = seqStart; i < 0x10000 * 5; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;

        ASSERT_TRUE(_srtp1->protect(*packet));

        if (i > 0x10000 + 100 && i < (0x10000 * 4 - 33000))
        {
            continue;
        }

        if (transport::SrtpClient::shouldSetRolloverCounter(unprotectedExtSeqNo, i))
        {
            logger::info("set roc %u", "", (i >> 16));
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, i >> 16)); // works without this due to continuous
            // sequence
        }
        _srtp2->unprotect(*packet);
        unprotectedExtSeqNo = i;
    }
}

TEST_F(SrtpTest, receive2VideoGap)
{
    setupDtls();
    connect();

    uint16_t seqStart = 16990;
    uint32_t unprotectedExtSeqNo = 0;

    for (uint32_t i = seqStart; i < 150000; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;

        ASSERT_TRUE(_srtp1->protect(*packet));
        const uint32_t contPoint = 82630u;
        // srtp must decode at least one packet while ROC is 0, otherwise we get err 7
        if (i < 17000)
        {
            continue;
        }
        if (i > 17011 && i < contPoint)
        {
            continue;
        }
        if (i > contPoint + 2 && i < 115999)
        {
            continue;
        }

        if (transport::SrtpClient::shouldSetRolloverCounter(unprotectedExtSeqNo, i))
        {
            logger::info("set roc %u. seq %u last unprotect %u", "", (i >> 16), i, unprotectedExtSeqNo);
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, i >> 16)); // works without this due to continuous

            // sequence
        }
        if (_srtp2->unprotect(*packet))
        {
            unprotectedExtSeqNo = i;
        }
    }
}

TEST_F(SrtpTest, muteUntilRoc1)
{
    setupDtls();
    connect();

    uint16_t seqStart = 65400;
    uint32_t unprotectedExtSeqNo = 0;

    for (uint32_t i = seqStart; i < 83000; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;

        ASSERT_TRUE(_srtp1->protect(*packet));
        const uint32_t contPoint = 82630u;
        // srtp must decode at least one packet while ROC is 0, otherwise we get err 7

        if (i < contPoint && i != seqStart)
        {
            continue;
        }

        if (transport::SrtpClient::shouldSetRolloverCounter(unprotectedExtSeqNo, i))
        {
            logger::info("set roc %u. seq %u last unprotect %u", "", (i >> 16), i, unprotectedExtSeqNo);
            EXPECT_TRUE(_srtp2->setRemoteRolloverCounter(1, i >> 16)); // works without this due to continuous

            // sequence
        }
        if (_srtp2->unprotect(*packet))
        {
            unprotectedExtSeqNo = i;
        }
    }
}

// sequence number starts close to 65535 and all packets with roc = 0 are lost.
// First packet seen is on ROC 1 already
TEST_F(SrtpTest, unprotect1st)
{
    setupDtls();
    connect();

    uint32_t roc = 0;

    auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
    auto header = rtp::RtpHeader::fromPacket(*packet);
    header->ssrc = 1;
    header->sequenceNumber = 670;
    header->timestamp = 160;
    const auto audioPacketLength = packet->getLength();
    const auto packetCopy = memory::makeUniquePacket(_allocator, *packet);

    ASSERT_TRUE(_srtp1->protect(*packet));

    ASSERT_TRUE(_srtp2->unprotectFirstRtp(*packet, roc));
    EXPECT_EQ(roc, 0);
    ASSERT_TRUE(packet->getLength() == audioPacketLength);
    EXPECT_EQ(0, std::memcmp(packet->get(), packetCopy->get(), packetCopy->getLength()));
}

// sequence number starts close to 65535 and all packets with roc = 0 are lost.
// First packet seen is on ROC 1 already
TEST_F(SrtpTest, losePacketsBeforeRoc1)
{
    setupDtls();
    connect();

    uint16_t seqStart = 65530;
    uint32_t unprotectedExtSeqNo = 0;
    uint32_t unprotectCount = 0;
    uint32_t roc = 0;
    for (uint32_t i = seqStart; i < 65601; ++i)
    {
        auto packet = memory::makeUniquePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->sequenceNumber = i & 0xFFFFu;
        header->timestamp = i * 160;
        const auto audioPacketLength = packet->getLength();

        ASSERT_TRUE(_srtp1->protect(*packet));
        const uint32_t continuationPoint = 65550;
        // srtp must decode at least one packet while ROC is 0, otherwise we get error

        if (i < continuationPoint)
        {
            continue;
        }

        if (_srtp2->unprotect(*packet))
        {
            unprotectedExtSeqNo = i;
            ++unprotectCount;
        }
        else if (unprotectCount == 0)
        {
            ASSERT_TRUE(_srtp2->unprotectFirstRtp(*packet, roc));
            EXPECT_EQ(roc, 1);
            unprotectedExtSeqNo = i;
            ++unprotectCount;
            ASSERT_TRUE(packet->getLength() == audioPacketLength);
        }
    }
    EXPECT_EQ(unprotectedExtSeqNo, 65600);
    EXPECT_EQ(unprotectCount, 51);
}
