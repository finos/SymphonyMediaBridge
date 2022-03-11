#include "memory/RingBuffer.h"
#include <array>
#include <gtest/gtest.h>

namespace
{

template <typename T>
void readAndValidate(T& ringBuffer, int16_t startValue, int16_t length)
{
    std::array<int16_t, 256> outData({});
    auto result = ringBuffer.read(outData.data(), length);
    ringBuffer.drop(length);
    EXPECT_TRUE(result);

    for (auto i = 0; i < length; ++i)
    {
        EXPECT_EQ(startValue + i, outData[i]);
    }
}

} // namespace

class RingbufferTest : public ::testing::Test
{
public:
    std::array<int16_t, 256> data;

    RingbufferTest() : data({}) {}

private:
    void SetUp() override
    {
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<int16_t>(i);
        }
    }

    void TearDown() override
    {
        // Code here will be called immediately after each test (right
        // before the destructor).
    }
};

TEST_F(RingbufferTest, readWrite)
{
    using namespace memory;

    RingBuffer<int16_t, 8> ringBuffer;

    bool result = ringBuffer.write(&data[0], 5);
    EXPECT_TRUE(result);

    result = ringBuffer.write(&data[5], 1);
    EXPECT_TRUE(result);

    readAndValidate(ringBuffer, 0, 4);

    result = ringBuffer.write(&data[6], 5);
    EXPECT_TRUE(result);

    readAndValidate(ringBuffer, 4, 6);

    result = ringBuffer.write(&data[11], 7);
    EXPECT_TRUE(result);

    readAndValidate(ringBuffer, 10, 8);

    result = ringBuffer.write(&data[18], 8);
    EXPECT_TRUE(result);

    readAndValidate(ringBuffer, 18, 8);
}

TEST_F(RingbufferTest, insertSilence)
{
    using namespace memory;

    RingBuffer<int16_t, 8> ringBuffer;

    auto result = ringBuffer.write(&data[0], 4);
    EXPECT_TRUE(result);

    ringBuffer.insertSilence(2);
    readAndValidate(ringBuffer, 0, 4);

    std::array<int16_t, 2> outData({1, 1});
    result = ringBuffer.read(outData.data(), 2);
    EXPECT_TRUE(result);
    EXPECT_EQ(0, outData[0]);
    EXPECT_EQ(0, outData[1]);
}

TEST_F(RingbufferTest, addToMix)
{
    using namespace memory;

    RingBuffer<int16_t, 8> ringBuffer;

    std::array<int16_t, 4> writeData({0, 2, 4, 6});

    auto result = ringBuffer.write(&writeData[0], 4);
    EXPECT_TRUE(result);

    std::array<int16_t, 4> mixedData({1, 1, 1, 1});
    result = ringBuffer.addToMix(mixedData.data(), 4, 2);
    EXPECT_TRUE(result);

    EXPECT_EQ(1, mixedData[0]);
    EXPECT_EQ(2, mixedData[1]);
    EXPECT_EQ(3, mixedData[2]);
    EXPECT_EQ(4, mixedData[3]);
}

TEST_F(RingbufferTest, removeFromMix)
{
    using namespace memory;

    RingBuffer<int16_t, 8> ringBuffer;

    std::array<int16_t, 4> writeData({0, 2, 4, 6});

    auto result = ringBuffer.write(&writeData[0], 4);
    EXPECT_TRUE(result);

    std::array<int16_t, 4> mixedData({1, 2, 3, 4});
    result = ringBuffer.removeFromMix(mixedData.data(), 4, 2);
    EXPECT_TRUE(result);

    EXPECT_EQ(1, mixedData[0]);
    EXPECT_EQ(1, mixedData[1]);
    EXPECT_EQ(1, mixedData[2]);
    EXPECT_EQ(1, mixedData[3]);
}
