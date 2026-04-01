#include "memory/PoolBuffer.h"
#include "memory/PoolAllocator.h"
#include "test/macros.h"
#include <gtest/gtest.h>
#include <vector>

TEST(PoolBuffer, create)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0);
    EXPECT_EQ(buffer.capacity(), 0);
}

TEST(PoolBuffer, allocateSmall)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(64));
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 64);
    EXPECT_EQ(buffer.capacity(), 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 1);
}

TEST(PoolBuffer, allocateExact)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(128));
    EXPECT_EQ(buffer.size(), 128);
    EXPECT_EQ(buffer.capacity(), 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 1);
}

TEST(PoolBuffer, allocateMultipleChunks)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(300));
    EXPECT_EQ(buffer.size(), 300);
    EXPECT_EQ(buffer.capacity(), 3 * 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 3);
}

TEST(PoolBuffer, allocateFail)
{
    memory::PoolAllocator<128> allocator(2, "test");
    const auto actualElementCount = allocator.size();
    const size_t sizeToRequest = actualElementCount * 128 + 1;

    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_FALSE(buffer.allocate(sizeToRequest));
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0);
    EXPECT_EQ(buffer.capacity(), 0);
    EXPECT_EQ(allocator.countAllocatedItems(), 0);
}

TEST(PoolBuffer, writeAndRead)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    const size_t dataSize = 300;
    EXPECT_TRUE(buffer.allocate(dataSize));

    std::vector<uint8_t> sourceData(dataSize);
    for (size_t i = 0; i < dataSize; ++i)
    {
        sourceData[i] = static_cast<uint8_t>(i);
    }

    EXPECT_EQ(buffer.write(sourceData.data(), sourceData.size()), dataSize);

    std::vector<uint8_t> destinationData(dataSize);
    auto reader = buffer.getReader();
    EXPECT_EQ(reader.read(destinationData.data(), destinationData.size()), dataSize);

    EXPECT_EQ(sourceData, destinationData);
}

TEST(PoolBuffer, writeAndReadWithOffset)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    const size_t bufferSize = 400;
    EXPECT_TRUE(buffer.allocate(bufferSize));

    std::vector<uint8_t> sourceData(150);
    for (size_t i = 0; i < sourceData.size(); ++i)
    {
        sourceData[i] = static_cast<uint8_t>(i);
    }

    const size_t writeOffset = 130; // Cross chunk boundary
    EXPECT_EQ(buffer.write(sourceData.data(), sourceData.size(), writeOffset), sourceData.size());

    std::vector<uint8_t> readData(sourceData.size());
    auto reader = buffer.getReader().subview(writeOffset, sourceData.size());
    EXPECT_EQ(reader.read(readData.data(), readData.size()), readData.size());

    EXPECT_EQ(sourceData, readData);
}

TEST(PoolBuffer, move)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer1(allocator);
    EXPECT_TRUE(buffer1.allocate(300));
    EXPECT_EQ(allocator.countAllocatedItems(), 3);

    memory::PoolBuffer<decltype(allocator)> buffer2 = std::move(buffer1);
    EXPECT_EQ(buffer2.size(), 300);
    EXPECT_EQ(buffer2.capacity(), 3 * 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 3);
    EXPECT_EQ(buffer1.size(), 0); // NOLINT

    buffer2.clear();
    EXPECT_EQ(allocator.countAllocatedItems(), 0);
}
