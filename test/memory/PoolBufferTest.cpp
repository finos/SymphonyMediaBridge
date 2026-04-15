#include "memory/PoolBuffer.h"
#include "memory/PoolAllocator.h"
#include "memory/Array.h"
#include "test/macros.h"
#include <gtest/gtest.h>
#include <vector>
#include <algorithm>

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

    // We expect 64 bytes totall fit into master chunk
    EXPECT_EQ(buffer.capacity(), 128 - sizeof(void*)); 
#if ENABLE_ALLOCATOR_METRICS
    EXPECT_EQ(allocator.countAllocatedItems(), 1);
#endif
}

TEST(PoolBuffer, allocateExact)
{
    // Leave space for single pointer to chunk[0] in the master chunk -
    // all data than should fit into the single buffer.
    memory::PoolAllocator<128 + sizeof(void*)> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(128));
    EXPECT_EQ(buffer.size(), 128);
    EXPECT_EQ(buffer.capacity(), 128);
#if ENABLE_ALLOCATOR_METRICS
    EXPECT_EQ(allocator.countAllocatedItems(), 1);
#endif
}

TEST(PoolBuffer, allocateMultipleChunks)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    EXPECT_TRUE(buffer.allocate(300));
    EXPECT_EQ(buffer.size(), 300);
    // 3 chunks needed: 2 full + 1 in the master chunk, slightly smaller
    EXPECT_EQ(buffer.capacity(), (3 - 1) * 128 + (128 - sizeof(void*) * 3));
#if ENABLE_ALLOCATOR_METRICS
    EXPECT_EQ(allocator.countAllocatedItems(), 3);
#endif
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
#if ENABLE_ALLOCATOR_METRICS
    EXPECT_EQ(allocator.countAllocatedItems(), 0);
#endif
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

    EXPECT_EQ(buffer.copyFrom(sourceData.data(), sourceData.size()), dataSize);

    char destinationData[dataSize];
    EXPECT_EQ(buffer.copyTo(destinationData, 0 , dataSize), dataSize);

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
    EXPECT_EQ(buffer.copyFrom(sourceData.data(), sourceData.size(), writeOffset), sourceData.size());

    char readData[150];
    EXPECT_EQ(buffer.copyTo(readData, writeOffset, sizeof(readData)), sizeof(readData));

    for (size_t i = 0; i < sourceData.size(); ++i)
    {
        EXPECT_EQ(sourceData[i], static_cast<uint8_t>(readData[i]));
    }
}

TEST(PoolBuffer, move)
{
// This test use allocator.countAllocatedItems extensively.
#if !ENABLE_ALLOCATOR_METRICS
    GTEST_SKIP();
#endif
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer1(allocator);
    EXPECT_TRUE(buffer1.allocate(300));
    EXPECT_EQ(allocator.countAllocatedItems(), 3);

    memory::PoolBuffer<decltype(allocator)> buffer2 = std::move(buffer1);
    EXPECT_EQ(buffer2.size(), 300);
    EXPECT_GE(buffer2.capacity(), 300);
    EXPECT_EQ(allocator.countAllocatedItems(), 3);
    EXPECT_EQ(buffer1.size(), 0); // NOLINT

    buffer2.clear();
    EXPECT_EQ(allocator.countAllocatedItems(), 0);
}

TEST(PoolBuffer, isNullTerminated)
{
    memory::PoolAllocator<128 + sizeof(void*)> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Empty buffer
    EXPECT_TRUE(buffer.allocate(0));
    EXPECT_FALSE(buffer.isNullTerminated());
    buffer.clear();
    EXPECT_FALSE(buffer.isNullTerminated());

    // Non-null terminated
    const std::string s1 = "123456789a";
    EXPECT_TRUE(buffer.allocate(s1.length()));
    buffer.copyFrom(s1.c_str(), s1.length());
    EXPECT_FALSE(buffer.isNullTerminated());

    // Null terminated
    const std::string s2 = "123456789";
    EXPECT_TRUE(buffer.allocate(s2.length() + 1));
    buffer.copyFrom(s2.c_str(), s2.length() + 1);
    EXPECT_TRUE(buffer.isNullTerminated());

    // Null at end of chunk
    std::vector<char> testData3(128, 'a');
    testData3[127] = '\0';
    EXPECT_TRUE(buffer.allocate(128));
    EXPECT_FALSE(buffer.isMultiChunk());
    buffer.copyFrom(testData3.data(), 128);
    EXPECT_TRUE(buffer.isNullTerminated());

    // Subview from larger buffer
    char testData4[] = {'1', '2', '3', '4', '5', '\0', '6', '7', '8', '\0', 'A'};
    EXPECT_TRUE(buffer.allocate(sizeof(testData4)));
    buffer.copyFrom(testData4, sizeof(testData4));
    EXPECT_FALSE(buffer.isNullTerminated());
}

TEST(PoolBuffer, deleter)
{
// This test use allocator.countAllocatedItems extensively.
#if !ENABLE_ALLOCATOR_METRICS
    GTEST_SKIP();
#endif
    memory::PoolAllocator<128> allocator(5, "test");
    EXPECT_EQ(allocator.countAllocatedItems(), 0);

     {
        memory::PoolBuffer<decltype(allocator)> buffer(allocator);
        EXPECT_TRUE(buffer.allocate(20));

        // Master chunk containing single pointer to data chunk 8 bytes,
        // datachunk payload itself (20 bytes) = 28.
        // All fits the single allocator's buffer of 128 bytes along with pointer.
        EXPECT_EQ(allocator.countAllocatedItems(), 1);
    }
    EXPECT_EQ(allocator.countAllocatedItems(), 0);

    {
        memory::PoolBuffer<decltype(allocator)> buffer(allocator);
        EXPECT_TRUE(buffer.allocate(3 * 128));
        EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1); // 3 chunks of 128 bytes + 1 'master chunk'
    }

    EXPECT_EQ(allocator.countAllocatedItems(), 0);

    memory::PoolBuffer<decltype(allocator)> buffer2(allocator);
    EXPECT_TRUE(buffer2.allocate(3 * 128));
    EXPECT_EQ(allocator.countAllocatedItems(), 3 + 1); // 3 chunks of 128 bytes + 1 'master chunk'
}

TEST(PoolBuffer, copyToAndNullTermination)
{
    memory::PoolAllocator<128> allocator(10, "test");
    memory::PoolBuffer<decltype(allocator)> buffer(allocator);

    // Empty buffer
    {
        EXPECT_TRUE(buffer.allocate(0));
        EXPECT_FALSE(buffer.isNullTerminated());
        EXPECT_FALSE(buffer.isMultiChunk());
        EXPECT_EQ(buffer.getLength(), 0);
    }

    // Single chunk, not null-terminated
    {
        const std::string testData = "single chunk test";
        EXPECT_TRUE(buffer.allocate(testData.length()));
        buffer.copyFrom(testData.c_str(), testData.length());
        EXPECT_FALSE(buffer.isNullTerminated());

        char readData[buffer.getLength()];
        EXPECT_EQ(buffer.getLength(), testData.length());
        EXPECT_EQ(buffer.copyTo(readData, 0, testData.length()), buffer.getLength());
        EXPECT_EQ(std::memcmp(readData, testData.c_str(), testData.length()), 0);
    }

    // Single chunk, null-terminated
    {
        const std::string testData = "single chunk nullterm";
        EXPECT_TRUE(buffer.allocate(testData.length() + 1));
        buffer.copyFrom(testData.c_str(), testData.length() + 1);
        EXPECT_TRUE(buffer.isNullTerminated());

        char readData[buffer.getLength()];
        EXPECT_EQ(buffer.getLength(), testData.length() + 1);
        EXPECT_EQ(buffer.copyTo(readData, 0, testData.length() + 1), buffer.getLength());
        EXPECT_EQ(std::string(readData), testData);
    }

    // Multi-chunk, not null-terminated
    {
        std::vector<char> testData(150, 'm');
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.copyFrom(testData.data(), testData.size());
        EXPECT_FALSE(buffer.isNullTerminated());

        char readData[buffer.getLength()];
        EXPECT_EQ(buffer.getLength(), testData.size());
        EXPECT_EQ(buffer.copyTo(readData, 0, testData.size()), buffer.getLength());
        EXPECT_EQ(std::memcmp(readData, testData.data(), testData.size()), 0);
    }

    // Multi-chunk, null-terminated
    {
        std::vector<char> testData(15, 'n');
        testData.back() = '\0';
        EXPECT_TRUE(buffer.allocate(testData.size()));
        buffer.copyFrom(testData.data(), testData.size());
        EXPECT_TRUE(buffer.isNullTerminated());

        char readData[buffer.getLength()];
        EXPECT_EQ(buffer.getLength(), testData.size());
        EXPECT_EQ(buffer.copyTo(readData, 0, testData.size()), buffer.getLength());
        EXPECT_EQ(std::memcmp(readData, testData.data(), testData.size()), 0);
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
    EXPECT_EQ(buffer.copyFrom(sourceData.data(), sourceData.size()), dataSize);

    // Test copying the full buffer
    std::vector<uint8_t> destData(dataSize);
    EXPECT_EQ(buffer.copyTo(destData.data(), 0, dataSize), dataSize);
    EXPECT_EQ(sourceData, destData);

    // Test copying a portion from the beginning
    std::fill(destData.begin(), destData.end(), 0);
    EXPECT_EQ(buffer.copyTo(destData.data(), 0, 100), 100);
    EXPECT_TRUE(std::equal(sourceData.begin(), sourceData.begin() + 100, destData.begin()));

    // Test copying a portion from the middle, crossing a chunk boundary
    std::fill(destData.begin(), destData.end(), 0);
    const size_t copyOffset = 100;
    const size_t copySize = 150;
    EXPECT_EQ(buffer.copyTo(destData.data(), copyOffset, copySize), copySize);
    EXPECT_TRUE(std::equal(sourceData.begin() + copyOffset, sourceData.begin() + copyOffset + copySize, destData.begin()));

    // Test copying with a count that goes over the end
    std::fill(destData.begin(), destData.end(), 0);
    EXPECT_EQ(buffer.copyTo(destData.data(), 200, 200), 100);
    EXPECT_TRUE(std::equal(sourceData.begin() + 200, sourceData.end(), destData.begin()));

    // Test copying with an offset that is out of bounds
    EXPECT_EQ(buffer.copyTo(destData.data(), dataSize, 1), 0);
    EXPECT_EQ(buffer.copyTo(destData.data(), dataSize + 1, 1), 0);

    // Test copying to a nullptr destination
    EXPECT_EQ(buffer.copyTo(nullptr, 0, 1), 0);

    // Test copying 0 bytes
    EXPECT_EQ(buffer.copyTo(destData.data(), 0, 0), 0);

    // Test copying from an empty buffer
    memory::PoolBuffer<decltype(allocator)> emptyBuffer(allocator);
    EXPECT_TRUE(emptyBuffer.allocate(0));
    EXPECT_EQ(emptyBuffer.copyTo(destData.data(), 0, 1), 0);
}

TEST(PoolBuffer, writeFromPoolBuffer)
{
    // Use different chunk sizes for src and dest to test more complex scenarios
    memory::PoolAllocator<100> srcAllocator(20, "srcTest");
    memory::PoolAllocator<128> destAllocator(20, "destTest");

    memory::PoolBuffer<decltype(srcAllocator)> srcBuffer(srcAllocator);
    srcBuffer.allocate(1000);
    memory::PoolBuffer<decltype(destAllocator)> destBuffer(destAllocator);
    destBuffer.allocate(1000);

    // Fill srcBuffer with some data
    std::vector<uint8_t> sourceData(1000);
    for (size_t i = 0; i < sourceData.size(); ++i)
    {
        sourceData[i] = static_cast<uint8_t>(i % 256);
    }
    srcBuffer.copyFrom(sourceData.data(), sourceData.size());

    // Case 1: Simple full copy
    {
        SCOPED_TRACE("Case 1: Simple full copy");
        std::vector<uint8_t> zeros(1000, 0);
        destBuffer.copyFrom(zeros.data(), zeros.size());

        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, 0, srcBuffer.size(), 0), srcBuffer.size());

        std::vector<uint8_t> destData(1000);
        destBuffer.copyTo(destData.data(), 0, destData.size());
        EXPECT_EQ(sourceData, destData);
    }

    // Case 2: Copy with source and destination offsets, crossing chunk boundaries
    {
        SCOPED_TRACE("Case 2: Offsets and boundary crossing");
        std::vector<uint8_t> zeros(1000, 0);
        destBuffer.copyFrom(zeros.data(), zeros.size());

        const size_t srcOffset = 50;   // start in 1st chunk of src
        const size_t destOffset = 150; // start in 2nd chunk of dest
        const size_t len = 300;        // will cross boundaries for both

        size_t bytesWritten = destBuffer.copyFrom(srcBuffer, srcOffset, len, destOffset);
        EXPECT_EQ(bytesWritten, len);

        // Check area before write
        std::vector<uint8_t> beforeData(destOffset);
        destBuffer.copyTo(beforeData.data(), 0, destOffset);
        EXPECT_TRUE(std::all_of(beforeData.begin(), beforeData.end(), [](uint8_t i) { return i == 0; }));

        // Check written data
        std::vector<uint8_t> writtenData(len);
        destBuffer.copyTo(writtenData.data(), destOffset, len);
        EXPECT_TRUE(std::equal(sourceData.begin() + srcOffset, sourceData.begin() + srcOffset + len, writtenData.begin()));

        // Check area after write
        const size_t afterOffset = destOffset + len;
        if (destBuffer.size() > afterOffset)
        {
            const size_t afterLen = destBuffer.size() - afterOffset;
            std::vector<uint8_t> afterData(afterLen);
            destBuffer.copyTo(afterData.data(), afterOffset, afterLen);
            EXPECT_TRUE(std::all_of(afterData.begin(), afterData.end(), [](uint8_t i) { return i == 0; }));
        }
    }

    // Case 3: Partial copy, len is smaller than available data
    {
        SCOPED_TRACE("Case 3: Partial copy");
        std::vector<uint8_t> zeros(1000, 0);
        destBuffer.copyFrom(zeros.data(), zeros.size());
        const size_t srcOffset = 10;
        const size_t destOffset = 20;
        const size_t len = 50;

        size_t bytesWritten = destBuffer.copyFrom(srcBuffer, srcOffset, len, destOffset);
        EXPECT_EQ(bytesWritten, len);

        std::vector<uint8_t> destData(len);
        destBuffer.copyTo(destData.data(), destOffset, len);
        EXPECT_TRUE(std::equal(sourceData.begin() + srcOffset, sourceData.begin() + srcOffset + len, destData.begin()));
    }

    // Case 4: len is larger than available in src
    {
        SCOPED_TRACE("Case 4: Read past source boundary");
        const size_t srcOffset = 900;
        const size_t destOffset = 0;
        const size_t len = 200; // only 100 bytes available in src
        const size_t expectedWrite = 100;

        size_t bytesWritten = destBuffer.copyFrom(srcBuffer, srcOffset, len, destOffset);
        EXPECT_EQ(bytesWritten, expectedWrite);

        std::vector<uint8_t> destData(expectedWrite);
        destBuffer.copyTo(destData.data(), destOffset, expectedWrite);
        EXPECT_TRUE(std::equal(sourceData.begin() + srcOffset, sourceData.end(), destData.begin()));
    }

    // Case 5: len is larger than available in dest
    {
        SCOPED_TRACE("Case 5: Write past destination boundary");
        const size_t srcOffset = 0;
        const size_t destOffset = 950;
        const size_t len = 100; // only 50 bytes available in dest
        const size_t expectedWrite = 50;

        size_t bytesWritten = destBuffer.copyFrom(srcBuffer, srcOffset, len, destOffset);
        EXPECT_EQ(bytesWritten, expectedWrite);

        std::vector<uint8_t> destData(expectedWrite);
        destBuffer.copyTo(destData.data(), destOffset, expectedWrite);
        EXPECT_TRUE(
            std::equal(sourceData.begin() + srcOffset, sourceData.begin() + srcOffset + expectedWrite, destData.begin()));
    }

    // Case 6: Zero-length copy
    {
        SCOPED_TRACE("Case 6: Zero-length copy");
        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, 10, 0, 10), 0);
    }

    // Case 7: Out-of-bounds offsets
    {
        SCOPED_TRACE("Case 7: Out-of-bounds offsets");
        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, srcBuffer.size(), 1, 0), 0);
        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, 0, 1, destBuffer.size()), 0);
        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, srcBuffer.size() + 1, 1, 0), 0);
        EXPECT_EQ(destBuffer.copyFrom(srcBuffer, 0, 1, destBuffer.size() + 1), 0);
    }

    // Case 8: Copy from empty source buffer
    {
        SCOPED_TRACE("Case 8: Empty source");
        memory::PoolBuffer<decltype(srcAllocator)> emptySrc(srcAllocator);
        emptySrc.allocate(0);
        EXPECT_EQ(destBuffer.copyFrom(emptySrc, 0, 1, 0), 0);
    }

    // Case 9: Copy to empty dest buffer
    {
        SCOPED_TRACE("Case 9: Empty destination");
        memory::PoolBuffer<decltype(destAllocator)> emptyDest(destAllocator);
        emptyDest.allocate(0);
        EXPECT_EQ(emptyDest.copyFrom(srcBuffer, 0, 1, 0), 0);
    }
}

