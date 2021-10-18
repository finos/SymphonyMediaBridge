#include "concurrency/LockFreeList.h"
#include "concurrency/ScopedSpinLocker.h"
#include "logger/Logger.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <queue>
#include <thread>
#include <unistd.h>
#include <utils/Time.h>
#include <vector>

using namespace concurrency;

const int BATCH_SIZE = 1000;
template <typename NodeType>
class DataItem : public NodeType
{
public:
    DataItem() = default;
    char data[90];
    void set(int n)
    {
        for (int i = 0; i < 90; ++i)
        {
            data[i] = n;
        }
    }
};

template <typename T, typename ItemType>
void workRun(T* list, std::atomic_bool* running)
{
    static_assert((sizeof(ItemType) % 8) == 0, "DataItem must be 64bit aligned");

    T tmp;
    int operations = 0;
    while (*running)
    {
        typename T::NodeType* item;
        int i;
        for (i = 0; i < BATCH_SIZE && list->pop(item); ++i)
        {
            ++operations;
            tmp.push(item);
        }

        int count = tmp.size();
        assert(count == i);
        while (tmp.pop(item))
        {
            --count;
            ++operations;
            list->push(item);
        }
        assert(count == 0);
    }
    logger::info("ops %d", "workRun", operations);
}

template <typename T>
void consistencyTest(const int threadCount, const int elementCount)
{
    std::atomic_bool running(true);
    T list;
    std::thread* prod[threadCount];
    DataItem<typename T::NodeType>* data = new DataItem<typename T::NodeType>[elementCount];
    for (int i = 0; i < threadCount; ++i)
    {
        prod[i] = new std::thread(workRun<T, DataItem<typename T::NodeType>>, &list, &running);
    }

    assert(reinterpret_cast<intptr_t>(data) != -1);
    for (int i = 0; i < elementCount; ++i)
    {
        data[i].set(i + 0x400);
        list.push(&data[i]);
        if (i % 100 == 0)
        {
            utils::Time::usleep(1000);
        }
    }

    const uint64_t ms = 1000;
    utils::Time::usleep(10000 * ms);
    running = false;
    for (int i = 0; i < threadCount; ++i)
    {
        prod[i]->join();
    }

    EXPECT_EQ(list.size(), elementCount);

    typename T::NodeType* item;
    int c = 0;
    for (c = 0; c < BATCH_SIZE * threadCount + 1; ++c)
    {
        if (!list.pop(item))
        {
            break;
        }
    }
    EXPECT_EQ(c, elementCount);

    for (int i = 0; i < threadCount; ++i)
    {
        delete prod[i];
    }

    delete[] data;
}

class QueueWrapper
{
public:
    typedef ListItem NodeType;
    void push(ListItem* item)
    {
        concurrency::ScopedSpinLocker lock(_mutex);
        _queue.push(item);
    }

    bool pop(ListItem*& item)
    {
        concurrency::ScopedSpinLocker lock(_mutex);
        if (_queue.empty())
        {
            return false;
        }

        item = _queue.front();
        _queue.pop();
        return true;
    }

    uint32_t size() { return _queue.size(); }
    std::atomic_flag _mutex = ATOMIC_FLAG_INIT;
    std::queue<ListItem*> _queue;
};

TEST(LFList, consistencyPlenty) { consistencyTest<LockFreeList>(8, 7 * BATCH_SIZE); }
TEST(LFList, consistencyFew) { consistencyTest<LockFreeList>(8, 128); }

TEST(LFList, plainConsistencyPlenty) { consistencyTest<QueueWrapper>(8, 7 * BATCH_SIZE); }
