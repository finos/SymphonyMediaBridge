#include "memory/RingAllocator.h"
#include "logger/Logger.h"
#include <gtest/gtest.h>
#include <list>

TEST(RingAllocatorTest, full)
{
    const auto MEM_SIZE = 256 * 1024;
    memory::RingAllocator mem(MEM_SIZE);

    std::list<void*> blocks;
    size_t totalSize = 0;
    for (int i = 0; i < 1000; ++i)
    {
        auto size = 16 + rand() % 4096;
        auto* p = mem.alloc(size);
        if (p == nullptr)
        {
            EXPECT_LT(totalSize, MEM_SIZE);
            EXPECT_LT(mem.capacity(), size + 16);

            break;
        }
        totalSize += size + 8;
        blocks.push_back(p);
    }

    while (!blocks.empty())
    {
        mem.free(blocks.front());
        blocks.erase(blocks.begin());
    }
}

TEST(RingAllocatorTest, tail)
{
    const auto MEM_SIZE = 256 * 1024;
    memory::RingAllocator mem(MEM_SIZE);

    std::list<void*> blocks;

    bool wrapped = false;
    for (int i = 0; i < 1000; ++i)
    {
        auto size = 16 + rand() % 4096;
        auto* p = mem.alloc(size);
        if (p == nullptr)
        {
            EXPECT_LT(mem.capacity(), 2 * size + 16);
        }
        else
        {
            if (p < blocks.back())
            {
                wrapped = true;
            }
            blocks.push_back(p);
        }

        if (i % 3 == 0)
        {
            mem.free(blocks.front());
            blocks.pop_front();
        }
    }
    EXPECT_TRUE(wrapped);
}

TEST(RingAllocatorTest, random)
{
    const auto MEM_SIZE = 256 * 1024;
    memory::RingAllocator mem(MEM_SIZE);

    std::list<void*> blocks;

    bool wrapped = false;
    for (int i = 0; i < 10000; ++i)
    {
        auto size = 16 + rand() % 4096;
        auto* p = mem.alloc(size);
        if (p == nullptr)
        {
            EXPECT_LT(mem.capacity(), size + 16);
        }
        else
        {
            std::memset(p, 0xFD, size);
            if (p < blocks.back())
            {
                wrapped = true;
            }
            blocks.push_back(p);
        }

        if (rand() % 5 == 0)
        {
            int pos = rand() % blocks.size();
            for (auto it = blocks.begin(); it != blocks.end(); ++it)
            {
                if (pos == 0)
                {
                    mem.free(*it);
                    blocks.erase(it);
                    break;
                }
                pos--;
            }
        }
    }
    EXPECT_TRUE(wrapped);
}

TEST(RingAllocatorTest, many)
{
    memory::RingAllocator mem(512 * 1024);
    std::vector<void*> blocks;

    for (int i = 0; i < 50000; ++i)
    {
        blocks.push_back(mem.alloc(77 + 34));
        blocks.push_back(mem.alloc(77 + 34));
        blocks.push_back(mem.alloc(77 + 34));

        mem.free(blocks[1]);
        mem.free(blocks[0]);
        mem.free(blocks[2]);

        blocks.clear();

        if (!mem.empty())
        {
            ASSERT_TRUE(mem.empty());
        }
    }
}