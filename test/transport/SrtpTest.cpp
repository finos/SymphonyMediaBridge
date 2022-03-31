#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/dtls/DtlsMessageListener.h"
#include "transport/dtls/SrtpClientFactory.h"
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

    ~FakeSrtpEndpoint()
    {
        for (memory::Packet* packet; _dtlsPackets.pop(packet);)
        {
            _allocator.free(packet);
        }
    }

    int32_t sendDtls(const char* buffer, uint32_t length) override
    {
        if (transport::isDtlsPacket(buffer))
        {
            _dtlsPackets.push(memory::makePacket(_allocator, buffer, length));
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
        for (memory::Packet* packet; _dtlsPackets.pop(packet);)
        {
            _peer.onMessageReceived(reinterpret_cast<char*>(packet->get()), packet->getLength());
            _allocator.free(packet);
        }
    }

    concurrency::MpmcQueue<memory::Packet*> _dtlsPackets;
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
        _factory = std::make_unique<transport::SrtpClientFactory>(*_dtls, _allocator);

        _srtp1 = _factory->create(this);
        _srtp2 = _factory->create(this);
        _ep1 = std::make_unique<FakeSrtpEndpoint>(*_srtp1, *_srtp2, _allocator);
        _ep2 = std::make_unique<FakeSrtpEndpoint>(*_srtp2, *_srtp1, _allocator);

        _srtp1->setRemoteDtlsFingerprint("sha-256", _dtls->getLocalFingerprint(), true);
        _srtp2->setRemoteDtlsFingerprint("sha-256", _dtls->getLocalFingerprint(), false);

        auto header = rtp::RtpHeader::create(_audioPacket);
        auto payload = header->getPayload();
        for (size_t i = 0; i < _audioPacket.getLength() - header->headerLength(); ++i)
        {
            payload[i] = i;
        }
    }

    void connect()
    {
        for (int i = 0; i < 500 && !(_srtp1->isDtlsConnected() && _srtp2->isDtlsConnected()); ++i)
        {
            _srtp1->processTimeout();
            _srtp2->processTimeout();
            _ep1->process();
            _ep2->process();
            utils::Time::nanoSleep(10 * utils::Time::ms);
        }
    }
    void onDtlsStateChange(transport::SrtpClient* srtpClient, transport::SrtpClient::State state) override
    {
        logger::info("DTLS state change %s", srtpClient->getLoggableId().c_str(), transport::toString(state));
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
    connect();

    uint16_t seqStart = 65530;
    for (int i = 0; i < 65535 * 4; ++i)
    {
        auto packet = memory::makePacket(_allocator, _audioPacket);
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->ssrc = 1;
        header->timestamp = i * 160;

        if (i == 90000)
        {
            i += 35000;
            _srtp1->setLocalRolloverCounter(1, 1 + (i >> 16));
        }
        if (i == 90000 + 35000 + 540)
        {
            _srtp2->setRemoteRolloverCounter(1, 1 + (i >> 16));
        }

        header->sequenceNumber = seqStart + i;

        EXPECT_TRUE(_srtp1->protect(packet));
        if (i >= 90000 + 35000 && i < 90000 + 35000 + 540)
        {
            EXPECT_FALSE(_srtp2->unprotect(packet));
        }
        else
        {
            EXPECT_TRUE(_srtp2->unprotect(packet));

            auto header = rtp::RtpHeader::fromPacket(*packet);
            EXPECT_TRUE(isDataValid(header->getPayload()));
            if ((i % 1000) == 0)
            {
                logger::debug("encrypted %u packets", "SrtpTest", i + 1);
            }
        }

        _allocator.free(packet);
    }
}

TEST_F(SrtpTest, seqDuplicate)
{
    connect();

    auto packet = memory::makePacket(_allocator, _audioPacket);
    auto header = rtp::RtpHeader::fromPacket(*packet);
    header->ssrc = 4321;
    header->timestamp = 1234;
    header->sequenceNumber = 5678;

    EXPECT_TRUE(_srtp1->protect(packet));

    auto packetCopy = memory::makePacket(_allocator, *packet);

    EXPECT_TRUE(_srtp2->unprotect(packet));
    EXPECT_FALSE(_srtp2->unprotect(packetCopy));

    _allocator.free(packetCopy);
    _allocator.free(packet);
}
