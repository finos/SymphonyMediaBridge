#include "bridge/engine/PacketCache.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

class PacketCacheTest : public ::testing::Test
{
    void SetUp() override
    {
        _packetAllocator = std::make_unique<memory::PacketPoolAllocator>(16, "PacketCacheTest");
        _packetCache = std::make_unique<bridge::PacketCache>("PacketCache", 1);
    }

    void TearDown() override
    {
        _packetCache.reset();
        _packetAllocator.reset();
    }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _packetAllocator;
    std::unique_ptr<bridge::PacketCache> _packetCache;

    memory::UniquePacket makeUniquePacket(const uint16_t sequenceNumber)
    {
        auto packet = memory::makeUniquePacket(*_packetAllocator);
        memset(packet->get(), 0, packet->size);
        reinterpret_cast<uint16_t*>(packet->get())[0] = sequenceNumber;
        packet->setLength(sizeof(uint16_t));
        return packet;
    }

    bool verifyPacket(const memory::Packet& packet, const uint16_t sequenceNumber)
    {
        return reinterpret_cast<const uint16_t*>(packet.get())[0] == sequenceNumber;
    }
};

TEST_F(PacketCacheTest, addPacket)
{
    auto packet = makeUniquePacket(1);
    EXPECT_TRUE(_packetCache->add(*packet, 1));

    auto cachedPacket = _packetCache->get(1);
    EXPECT_TRUE(verifyPacket(*cachedPacket, 1));
}

TEST_F(PacketCacheTest, packetAlreadyInCache)
{
    auto packet1 = makeUniquePacket(1);
    EXPECT_TRUE(_packetCache->add(*packet1, 1));

    auto packet2 = makeUniquePacket(1);
    EXPECT_FALSE(_packetCache->add(*packet2, 1));

    auto cachedPacket = _packetCache->get(1);
    EXPECT_TRUE(verifyPacket(*cachedPacket, 1));
}

TEST_F(PacketCacheTest, fillCache)
{
    for (auto i = 0; i < 512; ++i)
    {
        auto packet = makeUniquePacket(i);
        _packetCache->add(*packet, i);
    }

    auto packet = makeUniquePacket(256);
    EXPECT_TRUE(_packetCache->add(*packet, 512));

    auto packet2 = makeUniquePacket(513);
    EXPECT_TRUE(_packetCache->add(*packet2, 513));

    EXPECT_EQ(nullptr, _packetCache->get(0));
    EXPECT_NE(nullptr, _packetCache->get(512));
}
