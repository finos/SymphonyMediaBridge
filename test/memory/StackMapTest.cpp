#include "logger/Logger.h"
#include "memory/Map.h"
#include "test/concurrency/TestValues.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <utils/Time.h>

TEST(StackMap, fullCondition)
{
    memory::Map<int, Simple, 64> hmap;
    for (int i = 0; i < 67; ++i)
    {
        if (i < 64)
        {
            EXPECT_TRUE(hmap.add(i, Simple()).second);
        }
        else
        {
            EXPECT_FALSE(hmap.add(i, Simple()).second);
        }
    }
}

TEST(StackMap, emptyCondition)
{
    memory::Map<int, Simple, 64> hmap;
    EXPECT_TRUE(hmap.empty());
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_TRUE(hmap.add(i, Simple()).second);
    }
    EXPECT_EQ(hmap.size(), 64);
    for (int i = 0; i < 64; ++i)
    {
        auto it = hmap.find(i);
        if (it == hmap.end())
        {
            logger::debug("cannot find %d", "StackMapTest", i);
        }
        EXPECT_TRUE(it != hmap.cend());
    }
    for (int i = 0; i < 64; ++i)
    {
        hmap.erase(i);
    }
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_FALSE(hmap.contains(i));
    }
}

TEST(StackMap, iterate)
{
    memory::Map<int, Simple, 64> hmap;
    for (int i = 20; i < 50; ++i)
    {
        EXPECT_TRUE(hmap.add(i, Simple()).second);
    }

    const auto& constMap(hmap);

    auto it = constMap.find(45);
    EXPECT_TRUE(it != constMap.end());
    EXPECT_TRUE(hmap.contains(46));

    hmap.erase(42);
    EXPECT_FALSE(hmap.contains(42));

    int count = 0;
    for (auto& item : hmap)
    {
        EXPECT_GE(item.first, 20);
        EXPECT_LT(item.first, 50);
        ++count;
    }
    EXPECT_EQ(count, 29);

    EXPECT_TRUE(hmap.add(60, Simple(555, 999)).second);
    EXPECT_EQ(hmap[60].ssrc, 555);
}

TEST(StackMap, clear)
{
    memory::Map<int, Simple, 64> hmap;
    for (int i = 20; i < 50; ++i)
    {
        EXPECT_TRUE(hmap.add(i, Simple()).second);
    }

    hmap.clear();
    EXPECT_FALSE(hmap.contains(20));
    EXPECT_EQ(hmap.size(), 0);
    EXPECT_TRUE(hmap.empty());
    EXPECT_EQ(hmap.capacity(), 64);
}
