#include "memory/PoolBuffer.h"
#include "memory/PoolAllocator.h"
#include "memory/Array.h"
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
    EXPECT_EQ(allocator.countAllocatedItems(), 1 + 1);
}

TEST(PoolBuffer, allocateExact)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(128));
    EXPECT_EQ(buffer.size(), 128);
    EXPECT_EQ(buffer.capacity(), 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 1 + 1);
}

TEST(PoolBuffer, allocateMultipleChunks)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(300));
    EXPECT_EQ(buffer.size(), 300);
    EXPECT_EQ(buffer.capacity(), 3 * 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1);
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

    memory::Array<char, dataSize> destinationData;
    auto reader = buffer.getReader();
    EXPECT_EQ(reader.read(destinationData), dataSize);

    for (size_t i = 0; i < dataSize; ++i)
    {
        EXPECT_EQ(sourceData[i], static_cast<uint8_t>(destinationData[i]));
    }
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

    memory::Array<char, 150> readData;
    auto reader = buffer.getReader().subview(writeOffset, sourceData.size());
    EXPECT_EQ(reader.read(readData), readData.size());

    for (size_t i = 0; i < sourceData.size(); ++i)
    {
        EXPECT_EQ(sourceData[i], static_cast<uint8_t>(readData[i]));
    }
}

TEST(PoolBuffer, move)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer1(allocator);
    EXPECT_TRUE(buffer1.allocate(300));
    EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1);

    memory::PoolBuffer<decltype(allocator)> buffer2 = std::move(buffer1);
    EXPECT_EQ(buffer2.size(), 300);
    EXPECT_EQ(buffer2.capacity(), 3 * 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1);
    EXPECT_EQ(buffer1.size(), 0); // NOLINT

    buffer2.clear();
    EXPECT_EQ(allocator.countAllocatedItems(), 0);
}

TEST(PoolBuffer, isNullTerminated)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Empty buffer
    EXPECT_TRUE(buffer.allocate(0));
    EXPECT_FALSE(buffer.isNullTerminated());
    buffer.clear();
    EXPECT_FALSE(buffer.isNullTerminated());

    // Non-null terminated
    const std::string s1 = "123456789a";
    EXPECT_TRUE(buffer.allocate(s1.length()));
    buffer.write(s1.c_str(), s1.length());
    EXPECT_FALSE(buffer.isNullTerminated());

    // Null terminated
    const std::string s2 = "123456789";
    EXPECT_TRUE(buffer.allocate(s2.length() + 1));
    buffer.write(s2.c_str(), s2.length() + 1);
    EXPECT_TRUE(buffer.isNullTerminated());

    // Null at end of chunk
    std::vector<char> testData3(128, 'a');
    testData3[127] = '\0';
    EXPECT_TRUE(buffer.allocate(128));
    buffer.write(testData3.data(), 128);
    EXPECT_TRUE(buffer.isNullTerminated());
    EXPECT_FALSE(buffer.getReader().subview(0, 127).isNullTerminated());
    EXPECT_TRUE(buffer.getReader().subview(127, 1).isNullTerminated());
    EXPECT_FALSE(buffer.getReader().subview(126, 1).isNullTerminated());

    // Subview from larger buffer
    char testData4[] = {'1', '2', '3', '4', '5', '\0', '6', '7', '8', '\0', 'A'};
    EXPECT_TRUE(buffer.allocate(sizeof(testData4)));
    buffer.write(testData4, sizeof(testData4));
    EXPECT_FALSE(buffer.isNullTerminated());
    EXPECT_TRUE(buffer.getReader().subview(0, 6).isNullTerminated());
    EXPECT_FALSE(buffer.getReader().subview(0, 5).isNullTerminated());
    EXPECT_TRUE(buffer.getReader().subview(0, 10).isNullTerminated());
}

TEST(PoolBuffer, deleter)
{
    memory::PoolAllocator<128> allocator(5, "test");
    EXPECT_EQ(allocator.countAllocatedItems(), 0);

    {
        auto buffer = memory::makeUniquePoolBuffer(allocator, 3 * 128);
        EXPECT_TRUE(buffer);
        EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1 + 1); // 3 chunks of 128 bytes, 1 'master chunk', 1 for unuiq_ptr in-place new for PoolBuffer itself
    }

    EXPECT_EQ(allocator.countAllocatedItems(), 0);

    auto buffer2 = memory::makeUniquePoolBuffer(allocator, 3 * 128);
    EXPECT_TRUE(buffer2);
    EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1 + 1);
}

TEST(PoolBuffer, getReadonlyBuffer)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Empty buffer
    {
        EXPECT_TRUE(buffer.allocate(0));
        auto readonlyBuffer = buffer.getReadonlyBuffer();
        EXPECT_EQ(readonlyBuffer.data, nullptr);
        EXPECT_EQ(readonlyBuffer.length, 0);
    }

    // Single chunk, not null-terminated
    {
        const std::string testData = "single chunk test";
        EXPECT_TRUE(buffer.allocate(testData.length()));
        buffer.write(testData.c_str(), testData.length());
        EXPECT_FALSE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer();
        EXPECT_EQ(readonlyBuffer.length, testData.length());
        EXPECT_EQ(readonlyBuffer.storage, nullptr); // No copy should be made
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.c_str(), testData.length()), 0);
    }

    // Single chunk, null-terminated
    {
        const std::string testData = "single chunk nullterm";
        EXPECT_TRUE(buffer.allocate(testData.length() + 1));
        buffer.write(testData.c_str(), testData.length() + 1);
        EXPECT_TRUE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer();
        EXPECT_EQ(readonlyBuffer.length, testData.length() + 1);
        EXPECT_EQ(readonlyBuffer.storage, nullptr); // No copy should be made
        EXPECT_EQ(std::string(static_cast<const char*>(readonlyBuffer.data)), testData);
    }

    // Multi-chunk, not null-terminated
    {
        std::vector<char> testData(150, 'm');
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.write(testData.data(), testData.size());
        EXPECT_FALSE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer();
        EXPECT_NE(readonlyBuffer.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBuffer.length, testData.size());
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.data(), testData.size()), 0);
    }

    // Multi-chunk, null-terminated
    {
        std::vector<char> testData(150, 'n');
        testData.back() = '\0';
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.write(testData.data(), testData.size());
        EXPECT_TRUE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer();
        EXPECT_NE(readonlyBuffer.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBuffer.length, testData.size());
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.data(), testData.size()), 0);
    }
}

TEST(PoolBuffer, copy)
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

    // Test copying the full buffer
    std::vector<uint8_t> destData(dataSize);
    EXPECT_EQ(buffer.copy(destData.data(), 0, dataSize), dataSize);
    EXPECT_EQ(sourceData, destData);

    // Test copying a portion from the beginning
    std::fill(destData.begin(), destData.end(), 0);
    EXPECT_EQ(buffer.copy(destData.data(), 0, 100), 100);
    EXPECT_TRUE(std::equal(sourceData.begin(), sourceData.begin() + 100, destData.begin()));

    // Test copying a portion from the middle, crossing a chunk boundary
    std::fill(destData.begin(), destData.end(), 0);
    const size_t copyOffset = 100;
    const size_t copySize = 150;
    EXPECT_EQ(buffer.copy(destData.data(), copyOffset, copySize), copySize);
    EXPECT_TRUE(std::equal(sourceData.begin() + copyOffset, sourceData.begin() + copyOffset + copySize, destData.begin()));

    // Test copying with a count that goes over the end
    std::fill(destData.begin(), destData.end(), 0);
    EXPECT_EQ(buffer.copy(destData.data(), 200, 200), 100);
    EXPECT_TRUE(std::equal(sourceData.begin() + 200, sourceData.end(), destData.begin()));

    // Test copying with an offset that is out of bounds
    EXPECT_EQ(buffer.copy(destData.data(), dataSize, 1), 0);
    EXPECT_EQ(buffer.copy(destData.data(), dataSize + 1, 1), 0);

    // Test copying to a nullptr destination
    EXPECT_EQ(buffer.copy(nullptr, 0, 1), 0);

    // Test copying 0 bytes
    EXPECT_EQ(buffer.copy(destData.data(), 0, 0), 0);

    // Test copying from an empty buffer
    memory::PoolBuffer<decltype(allocator)> emptyBuffer(allocator);
    EXPECT_TRUE(emptyBuffer.allocate(0));
    EXPECT_EQ(emptyBuffer.copy(destData.data(), 0, 1), 0);
}
