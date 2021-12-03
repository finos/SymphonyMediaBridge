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

TEST(BacklogTest, complex)
{
    struct AllocObject
    {
        AllocObject() {}
        AllocObject(const AllocObject&) = delete;
        int mo = 99;
    } mainAllocator;

    struct Complicated
    {
        AllocObject& allocator;
        int* p;
    };

    memory::RandomAccessBacklog<Complicated, 64> backlog;

    EXPECT_TRUE(backlog.empty());
    int test1 = 88;

    AllocObject& mainRef(mainAllocator);
    for (int i = 0; i < 64; ++i)
    {
        backlog.push_front({mainRef, &test1});
        EXPECT_EQ(backlog.size(), i + 1);
    }
    EXPECT_TRUE(backlog.full());

    Complicated c = backlog.back();
    backlog.pop_back();
    EXPECT_EQ(*c.p, 88);

    while (!backlog.empty())
    {
        backlog.pop_back();
    }
    EXPECT_EQ(backlog.size(), 0);
}
