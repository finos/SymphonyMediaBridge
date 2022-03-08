#include "bridge/engine/PacketCache.h"
#include "memory/RefCountedPacket.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace
{

const size_t iterations = 2048;

void threadFunctionAdd(bridge::PacketCache* packetCache, memory::PacketPoolAllocator* allocator)
{
    for (size_t sequenceNumber = 0; sequenceNumber < iterations; ++sequenceNumber)
    {
        // Local packet that will be copied to the cache
        auto packet = memory::makePacket(*allocator);
        EXPECT_NE(nullptr, packet);

        // Write sequence number at the start of the packet
        auto intptr = reinterpret_cast<uint32_t*>(packet->get());
        *intptr = sequenceNumber;
        packet->setLength(sizeof(uint32_t));

        packetCache->add(packet, sequenceNumber);

        // Done with our local packet since it's copied to the cache
        allocator->free(packet);
    }
}

void threadFunctionGet(bridge::PacketCache* packetCache)
{
    for (size_t sequenceNumber = 0; sequenceNumber < iterations; ++sequenceNumber)
    {
        auto packet = packetCache->get(sequenceNumber);
        if (!packet)
        {
            usleep(10);
            continue;
        }

        // Check packet contents, should match the sequence number written as payload
        const auto payload = packet->get();
        auto intptr = reinterpret_cast<uint32_t*>(payload->get());
        EXPECT_EQ(sizeof(uint32_t), payload->getLength());
        EXPECT_EQ(sequenceNumber, *intptr);

        // Re-check packet contents so it's not overwritten while we have a reference to it
        usleep(100);
        EXPECT_EQ(sequenceNumber, *intptr);

        packet->release();
    }
}

} // namespace

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

    memory::Packet* makePacket(const uint16_t sequenceNumber)
    {
        auto packet = memory::makePacket(*_packetAllocator);
        memset(packet->get(), 0, packet->size);
        reinterpret_cast<uint16_t*>(packet->get())[0] = sequenceNumber;
        packet->setLength(sizeof(uint16_t));
        return packet;
    }

    bool verifyPacket(const memory::Packet* packet, const uint16_t sequenceNumber)
    {
        return reinterpret_cast<const uint16_t*>(packet->get())[0] == sequenceNumber;
    }
};

TEST_F(PacketCacheTest, addPacket)
{
    auto packet = makePacket(1);
    EXPECT_TRUE(_packetCache->add(packet, 1));
    _packetAllocator->free(packet);

    auto cachedPacket = _packetCache->get(1);
    EXPECT_TRUE(verifyPacket(cachedPacket->get(), 1));
}

TEST_F(PacketCacheTest, packetAlreadyInCache)
{
    auto packet1 = makePacket(1);
    EXPECT_TRUE(_packetCache->add(packet1, 1));
    _packetAllocator->free(packet1);

    auto packet2 = makePacket(1);
    EXPECT_FALSE(_packetCache->add(packet2, 1));
    _packetAllocator->free(packet2);

    auto cachedPacket = _packetCache->get(1);
    EXPECT_TRUE(verifyPacket(cachedPacket->get(), 1));
}

TEST_F(PacketCacheTest, fillCache)
{
    for (auto i = 0; i < 512; ++i)
    {
        auto packet = makePacket(i);
        _packetCache->add(packet, i);
        _packetAllocator->free(packet);
    }

    auto packet = makePacket(256);
    EXPECT_TRUE(_packetCache->add(packet, 512));
    _packetAllocator->free(packet);

    EXPECT_EQ(nullptr, _packetCache->get(0));
    EXPECT_NE(nullptr, _packetCache->get(512));
}

TEST_F(PacketCacheTest, multiThread)
{
    auto addThread = std::make_unique<std::thread>(threadFunctionAdd, _packetCache.get(), _packetAllocator.get());
    auto getThread = std::make_unique<std::thread>(threadFunctionGet, _packetCache.get());

    addThread->join();
    getThread->join();
}
