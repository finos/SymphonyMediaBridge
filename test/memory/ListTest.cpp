#include "memory/List.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>

namespace
{

const size_t listSize = 1024;
using TestList = memory::List<uint32_t, listSize>;

bool existsInList(TestList& list, const uint32_t a)
{
    auto entry = list.head();
    while (entry)
    {
        if (entry->_data == a)
        {
            return true;
        }
        entry = entry->_next;
    }

    return false;
}

} // namespace

class ListTest : public ::testing::Test
{
public:
    ListTest() {}

private:
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(ListTest, addRemove)
{
    TestList list;

    EXPECT_TRUE(list.pushToTail(0));
    EXPECT_TRUE(list.pushToTail(1));
    EXPECT_TRUE(list.remove(0));
    EXPECT_TRUE(list.pushToTail(2));
    EXPECT_TRUE(list.remove(1));
    EXPECT_TRUE(list.remove(2));
    EXPECT_TRUE(list.pushToTail(3));
    EXPECT_TRUE(list.remove(3));

    EXPECT_EQ(nullptr, list.head());
}

TEST_F(ListTest, addFindRemove)
{
    TestList list;

    for (uint32_t count = 0; count < 16; ++count)
    {
        std::vector<uint32_t> entries;

        // Fill list
        for (uint32_t i = 0; i < listSize; ++i)
        {
            entries.push_back(i);
            EXPECT_TRUE(list.pushToTail(i));
        }

        // Unable to add more elements than listSize
        EXPECT_FALSE(list.pushToTail(listSize));

        const uint32_t halfListSize = listSize / 2;

        // Remove half of the entries randomly
        std::random_device randomDevice;
        std::mt19937 generator(randomDevice());
        std::shuffle(entries.begin(), entries.end(), generator);
        for (uint32_t i = 0; i < halfListSize; ++i)
        {
            const uint32_t entry = entries.back();
            entries.pop_back();
            EXPECT_TRUE(existsInList(list, entry));
            EXPECT_TRUE(list.remove(entry));
            EXPECT_FALSE(existsInList(list, entry));
        }

        // Add new entries to fill the list
        for (uint32_t i = 0; i < halfListSize; ++i)
        {
            uint32_t entry = listSize + i;
            entries.push_back(entry);
            EXPECT_TRUE(list.pushToTail(entry));
        }

        // Remove all entries
        for (auto entry : entries)
        {
            EXPECT_TRUE(existsInList(list, entry));
            EXPECT_TRUE(list.remove(entry));
            EXPECT_FALSE(existsInList(list, entry));
        }

        EXPECT_EQ(nullptr, list.head());
    }
}

TEST_F(ListTest, addPop)
{
    TestList list;

    EXPECT_TRUE(list.pushToTail(0));
    EXPECT_TRUE(list.pushToTail(1));

    uint32_t outData = 0xFFFFFFFF;
    EXPECT_TRUE(list.popFromHead(outData));
    EXPECT_EQ(0, outData);

    EXPECT_FALSE(list.remove(0));
    EXPECT_TRUE(list.remove(1));

    EXPECT_EQ(nullptr, list.head());
    EXPECT_EQ(nullptr, list.tail());
}
