#include "memory/PriorityQueue.h"
#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace
{

static const size_t queueSize = 16384;
using TestPriorityQueue = memory::PriorityQueue<uint32_t, queueSize>;

} // namespace

class PriorityQueueTest : public ::testing::Test
{
public:
    PriorityQueueTest() = default;

private:
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(PriorityQueueTest, topReturnsMax)
{
    TestPriorityQueue priorityQueue;

    priorityQueue.push(1);
    priorityQueue.push(2);
    priorityQueue.push(3);

    EXPECT_EQ(3, priorityQueue.top());
}

TEST_F(PriorityQueueTest, popRemovesMax)
{
    TestPriorityQueue priorityQueue;

    priorityQueue.push(1);
    priorityQueue.push(2);
    priorityQueue.push(3);

    priorityQueue.pop();
    EXPECT_EQ(2, priorityQueue.top());

    priorityQueue.pop();
    EXPECT_EQ(1, priorityQueue.top());

    priorityQueue.pop();
    EXPECT_TRUE(priorityQueue.empty());
}

TEST_F(PriorityQueueTest, pushPop)
{
    TestPriorityQueue priorityQueue;

    std::vector<uint32_t> values;

    std::random_device randomDevice;
    std::mt19937 generator(randomDevice());
    std::uniform_int_distribution<uint32_t> distribution(0, std::numeric_limits<uint32_t>::max());

    for (size_t i = 0; i < queueSize; ++i)
    {
        values.push_back(distribution(generator));
    }

    for (const auto value : values)
    {
        priorityQueue.push(value);
    }

    std::sort(values.begin(), values.end(), [](const auto left, const auto right) { return left > right; });
    for (const auto value : values)
    {
        EXPECT_EQ(value, priorityQueue.top());
        priorityQueue.pop();
    }

    EXPECT_TRUE(priorityQueue.empty());
}
