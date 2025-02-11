#include "concurrency/MpmcQueue.h"
#include "TestValues.h"
#include <gtest/gtest.h>

using namespace concurrency;

TEST(MpmcQueue, zeroElements)
{
    MpmcQueue<Simple> queue(0);

    Simple v;

    EXPECT_EQ(0, queue.size());
    EXPECT_EQ(true, queue.full());
    EXPECT_EQ(true, queue.empty());

    EXPECT_EQ(false, queue.pop(v));

    EXPECT_EQ(false, queue.push());
    EXPECT_EQ(false, queue.push(v));

    EXPECT_EQ(0, queue.size());
    EXPECT_EQ(true, queue.full());
    EXPECT_EQ(true, queue.empty());

    EXPECT_EQ(false, queue.pop(v));

    EXPECT_EQ(0, queue.size());
    EXPECT_EQ(true, queue.full());
    EXPECT_EQ(true, queue.empty());
}
