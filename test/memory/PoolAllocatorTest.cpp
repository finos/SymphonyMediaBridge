#include "memory/PoolAllocator.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "memory/RefCountedPacket.h"
#include "test/bridge/DummyRtcTransport.h"
#include "test/macros.h"
#include "utils/Time.h"
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>
namespace
{

const size_t numThreads = 32;
const size_t iterations = (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)) ? 500 : 10000;

struct Data
{
    char data[4096];
};

using TestAllocator = memory::PoolAllocator<sizeof(Data)>;

void threadFunction(TestAllocator* allocator, const uint32_t id)
{
    std::default_random_engine generator(id);
    std::uniform_int_distribution<useconds_t> distribution(0, 10);

    for (uint32_t i = 0; i < iterations; ++i)
    {
        usleep(distribution(generator));
        Data* allocation = reinterpret_cast<Data*>(allocator->allocate());
        EXPECT_TRUE(allocation);
        memset(allocation, id, sizeof(Data));

        usleep(distribution(generator));
        for (auto j = 0; j < 4096; ++j)
        {
            EXPECT_EQ(static_cast<char>(id), allocation->data[j]);
        }
        allocator->free(allocation);
    }
}

} // namespace

class PoolAllocatorTest : public ::testing::Test
{
    void SetUp() override
    {
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    void TearDown() override
    {
        // Code here will be called immediately after each test (right
        // before the destructor).
    }
};

TEST_F(PoolAllocatorTest, singleThreaded)
{
    auto allocator = std::make_unique<memory::PoolAllocator<sizeof(Data)>>(3, "PoolAllocatorTest");

    Data* data0 = reinterpret_cast<Data*>(allocator->allocate());
    memset(data0, 1, sizeof(Data));
    Data* data1 = reinterpret_cast<Data*>(allocator->allocate());
    memset(data1, 2, sizeof(Data));
    Data* data2 = reinterpret_cast<Data*>(allocator->allocate());
    memset(data2, 3, sizeof(Data));

    EXPECT_EQ(nullptr, allocator->allocate());

    allocator->free(data1);
    allocator->free(data0);
    allocator->free(data2);
}

TEST_F(PoolAllocatorTest, multiThreaded)
{
    auto allocator = std::make_unique<TestAllocator>(1024, "PoolAllocatorTest");
    std::vector<std::unique_ptr<std::thread>> threads;
    for (size_t i = 0; i < numThreads; ++i)
    {
        threads.emplace_back(std::make_unique<std::thread>(threadFunction, allocator.get(), i));
    }

    for (size_t i = 0; i < numThreads; ++i)
    {
        threads[i]->join();
    }
}

namespace
{
void performanceTest(TestAllocator* allocator, int id, std::atomic_bool* running)
{
    std::default_random_engine generator(id);
    std::uniform_int_distribution<useconds_t> distribution(0, 10);

    size_t iterations = 0;
    size_t misses = 0;
    Data* items[2000];
    std::memset(items, 0, 2000 * sizeof(Data*));
    int i = 0;
    while (*running)
    {
        if (items[i] != 0)
        {
            allocator->free(items[i]);
        }
        items[i] = reinterpret_cast<Data*>(allocator->allocate());

        i = (i + 1) % 2000;
        ++iterations;
    }
    logger::info("mips %zu, misses %zu", "performanceThread", iterations, misses);
    for (int i = 0; i < 2000; ++i)
    {
        allocator->free(items[i]);
    }
}
} // namespace

TEST_F(PoolAllocatorTest, performance)
{
    const int THREADS = 8;
    TestAllocator allocator1(4096 * 40, "PoolAllocatorTest");
    std::vector<std::unique_ptr<std::thread>> threads;
    std::atomic_bool running(true);

    for (auto i = 0; i < THREADS; ++i)
    {
        if (i % 2 == 0)
        {
            threads.emplace_back(std::make_unique<std::thread>(
                [&allocator1, i, &running] { performanceTest(&allocator1, i, &running); }));
        }
        else
        {
            threads.emplace_back(std::make_unique<std::thread>(
                [&allocator1, i, &running] { performanceTest(&allocator1, i, &running); }));
        }
    }

    utils::Time::nanoSleep(utils::Time::sec * 4);
    running = false;
    logger::info("stopping test", "PoolAllocatorTest.performance");
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i]->join();
    }
}

TEST(PoolAllocatorBasic, leakReport)
{
    {
        TestAllocator allocator(4096 * 40, "PoolAllocatorTest");

        allocator.allocate();
        allocator.allocate();
    }
}

TEST(PoolAllocatorBasic, refCountedPacket)
{
    {
        memory::PacketPoolAllocator allocator(4096 * 40, "PoolAllocatorTest");

        size_t count = 0;
        auto start = utils::Time::getAbsoluteTime();
        while (utils::Time::getAbsoluteTime() - start < utils::Time::sec * 4)
        {
            memory::RefCountedPacket rpacket(memory::makePacket(allocator), &allocator);
            rpacket.get()->setLength(1);
            count += rpacket.get()->getLength();
            rpacket.release();
        }
        logger::info("ops %zu", "refCountedPacket", count);
    }
}

namespace
{

template <typename PacketPtrT>
struct IncomingPacketAggregate
{
    IncomingPacketAggregate() : _transport(nullptr), _extendedSequenceNumber(0) {}

    IncomingPacketAggregate(PacketPtrT packet, transport::RtcTransport* transport)
        : _packet(std::move(packet)),
          _transport(transport),
          _extendedSequenceNumber(0)
    {
    }

    IncomingPacketAggregate(PacketPtrT packet,
        transport::RtcTransport* transport,
        const uint32_t extendedSequenceNumber)
        : _packet(std::move(packet)),
          _transport(transport),
          _extendedSequenceNumber(extendedSequenceNumber)
    {
    }

    PacketPtrT _packet;
    transport::RtcTransport* _transport;
    uint32_t _extendedSequenceNumber;

    inline void lockOwner() const
    {
        if (_transport)
        {
            ++_transport->getJobCounter();
        }
    }

    inline void release() const
    {
        if (_transport)
        {
#if DEBUG
            const auto decreased = --_transport->getJobCounter();
            assert(decreased < 0xFFFFFFFF); // detecting going below zero
#else
            --_transport->getJobCounter();
#endif
        }
        _packet.release();
    }
};
} // namespace

TEST(PoolAllocatorBasic, deleter)
{
    memory::PacketPoolAllocator allocator(1024, "mypackets");
    memory::PacketPoolAllocator allocator2(512, "twopack");

    auto packet = memory::makeUniquePacket(allocator);
    auto packet2 = memory::makeUniquePacket(allocator2);

    concurrency::MpmcQueue<memory::UniquePacket> queue(1024);

    memory::UniquePacket clean;
    clean = std::move(packet2);
    queue.push(std::move(packet));
    packet = std::move(clean);

    auto otherPacket = std::move(packet);

    auto packet3 = memory::makeUniquePacket(allocator);
    IncomingPacketAggregate<memory::UniquePacket> aggr(std::move(packet3), nullptr);
    concurrency::MpmcQueue<IncomingPacketAggregate<memory::UniquePacket>> recvQueue(64);

    recvQueue.push(std::move(aggr));
    IncomingPacketAggregate<memory::UniquePacket> aggr2;
    EXPECT_TRUE(recvQueue.pop(aggr2));
}
