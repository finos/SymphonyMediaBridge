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
    EXPECT_EQ(allocator.countAllocatedItems(), 3);

    memory::PoolBuffer<decltype(allocator)> buffer2 = std::move(buffer1);
    EXPECT_EQ(buffer2.size(), 300);
    EXPECT_EQ(buffer2.capacity(), 3 * 128);
    EXPECT_EQ(allocator.countAllocatedItems(), 3);
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

TEST(PoolBuffer, readAndAppendNullIfNeeded)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Not null-terminated, enough space
    {
        const std::string testData = "0123456789";
        EXPECT_TRUE(buffer.allocate(testData.length()));
        buffer.write(testData.c_str(), testData.length());

        memory::Array<char, 20> dest;
        auto reader = buffer.getReader();
        EXPECT_EQ(reader.readAndAppendNullIfNeeded(dest), testData.length() + 1);
        EXPECT_EQ(std::string(dest.data()), testData);
        EXPECT_EQ(dest[testData.length()], '\0');
    }

    // Null-terminated, enough space
    {
        const std::string testData = "0123456789";
        EXPECT_TRUE(buffer.allocate(testData.length() + 1));
        buffer.write(testData.c_str(), testData.length() + 1);

        memory::Array<char, 20> dest;
        auto reader = buffer.getReader();
        EXPECT_EQ(reader.readAndAppendNullIfNeeded(dest), testData.length() + 1);
        EXPECT_EQ(std::string(dest.data()), testData);
    }

    // Not null-terminated, not enough space for null
    {
        const std::string testData = "0123456789";
        EXPECT_TRUE(buffer.allocate(testData.length()));
        buffer.write(testData.c_str(), testData.length());
        memory::Array<char, 10> dest;
        auto reader = buffer.getReader();
        EXPECT_EQ(reader.readAndAppendNullIfNeeded(dest), 0);
    }

    // Null-terminated, not enough space for content
    {
        const std::string testData = "0123456789";
        EXPECT_TRUE(buffer.allocate(testData.length() + 1));
        buffer.write(testData.c_str(), testData.length() + 1);
        memory::Array<char, 10> dest;
        auto reader = buffer.getReader();
        EXPECT_EQ(reader.readAndAppendNullIfNeeded(dest), 0);
    }

    // Multi-chunk and not null terminated
    {
        std::vector<char> testData(130, 'a');
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.write(testData.data(), testData.size());
        EXPECT_FALSE(buffer.isNullTerminated());

        memory::Array<char, 200> dest;
        auto reader = buffer.getReader();
        EXPECT_EQ(reader.readAndAppendNullIfNeeded(dest), testData.size() + 1);
        EXPECT_EQ(dest[testData.size()], '\0');
        EXPECT_EQ(std::memcmp(dest.data(), testData.data(), testData.size()), 0);
    }
}

TEST(PoolBuffer, getReadonlyBuffer)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Empty buffer
    {
        EXPECT_TRUE(buffer.allocate(0));
        auto readonlyBuffer = buffer.getReadonlyBuffer(memory::ReadMode::AsIs);
        EXPECT_EQ(readonlyBuffer.data, nullptr);
        EXPECT_EQ(readonlyBuffer.length, 0);

        auto readonlyBufferNT = buffer.getReadonlyBuffer(memory::ReadMode::NullTerminated);
        EXPECT_NE(readonlyBufferNT.data, nullptr);
        EXPECT_EQ(readonlyBufferNT.length, 1);
        EXPECT_EQ(static_cast<const char*>(readonlyBufferNT.data)[0], '\0');
    }

    // Single chunk, not null-terminated
    {
        const std::string testData = "single chunk test";
        EXPECT_TRUE(buffer.allocate(testData.length()));
        buffer.write(testData.c_str(), testData.length());
        EXPECT_FALSE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer(memory::ReadMode::AsIs);
        EXPECT_EQ(readonlyBuffer.length, testData.length());
        EXPECT_EQ(readonlyBuffer.storage, nullptr); // No copy should be made
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.c_str(), testData.length()), 0);

        auto readonlyBufferNT = buffer.getReadonlyBuffer(memory::ReadMode::NullTerminated);
        EXPECT_NE(readonlyBufferNT.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBufferNT.length, testData.length() + 1);
        EXPECT_EQ(std::string(static_cast<const char*>(readonlyBufferNT.data)), testData);
    }

    // Single chunk, null-terminated
    {
        const std::string testData = "single chunk nullterm";
        EXPECT_TRUE(buffer.allocate(testData.length() + 1));
        buffer.write(testData.c_str(), testData.length() + 1);
        EXPECT_TRUE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer(memory::ReadMode::AsIs);
        EXPECT_EQ(readonlyBuffer.length, testData.length() + 1);
        EXPECT_EQ(readonlyBuffer.storage, nullptr); // No copy should be made
        EXPECT_EQ(std::string(static_cast<const char*>(readonlyBuffer.data)), testData);

        auto readonlyBufferNT = buffer.getReadonlyBuffer(memory::ReadMode::NullTerminated);
        EXPECT_EQ(readonlyBufferNT.length, testData.length() + 1);
        EXPECT_EQ(readonlyBufferNT.storage, nullptr); // No copy should be made
        EXPECT_EQ(std::string(static_cast<const char*>(readonlyBufferNT.data)), testData);
    }

    // Multi-chunk, not null-terminated
    {
        std::vector<char> testData(150, 'm');
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.write(testData.data(), testData.size());
        EXPECT_FALSE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer(memory::ReadMode::AsIs);
        EXPECT_NE(readonlyBuffer.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBuffer.length, testData.size());
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.data(), testData.size()), 0);

        auto readonlyBufferNT = buffer.getReadonlyBuffer(memory::ReadMode::NullTerminated);
        EXPECT_NE(readonlyBufferNT.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBufferNT.length, testData.size() + 1);
        EXPECT_EQ(static_cast<const char*>(readonlyBufferNT.data)[testData.size()], '\0');
        EXPECT_EQ(std::memcmp(readonlyBufferNT.data, testData.data(), testData.size()), 0);
    }

    // Multi-chunk, null-terminated
    {
        std::vector<char> testData(150, 'n');
        testData.back() = '\0';
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.write(testData.data(), testData.size());
        EXPECT_TRUE(buffer.isNullTerminated());

        auto readonlyBuffer = buffer.getReadonlyBuffer(memory::ReadMode::AsIs);
        EXPECT_NE(readonlyBuffer.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBuffer.length, testData.size());
        EXPECT_EQ(std::memcmp(readonlyBuffer.data, testData.data(), testData.size()), 0);

        auto readonlyBufferNT = buffer.getReadonlyBuffer(memory::ReadMode::NullTerminated);
        EXPECT_NE(readonlyBufferNT.storage, nullptr); // Should be a copy
        EXPECT_EQ(readonlyBufferNT.length, testData.size());
        EXPECT_EQ(std::memcmp(readonlyBufferNT.data, testData.data(), testData.size()), 0);
    }
}
