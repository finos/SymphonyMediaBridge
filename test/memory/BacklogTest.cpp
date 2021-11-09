#include "memory/RandomAccessBacklog.h"
#include <gtest/gtest.h>

TEST(BacklogTest, addRemove)
{
    memory::RandomAccessBacklog<uint32_t, 64> backlog;

    EXPECT_TRUE(backlog.empty());

    for (int i = 0; i < 64; ++i)
    {
        backlog.push_front(i + 1);
        EXPECT_EQ(backlog.size(), i + 1);
    }

    EXPECT_EQ(backlog[63], 1);
    EXPECT_EQ(backlog[0], 64);

    int itemCount = 0;
    for (auto& value : backlog)
    {
        EXPECT_EQ(value, 64 - itemCount);
        ++itemCount;
    }

    EXPECT_EQ(itemCount, 64);

    backlog.emplace_front(65);
    EXPECT_EQ(backlog.size(), 64);

    EXPECT_EQ(backlog[0], 65);
    EXPECT_EQ(backlog.front(), 65);
    EXPECT_EQ(backlog.back(), 2);
}
