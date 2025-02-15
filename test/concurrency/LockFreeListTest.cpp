#include "concurrency/LockFreeList.h"
#include "concurrency/MpmcQueue.h"
#include "concurrency/ScopedSpinLocker.h"
#include "logger/Logger.h"
#include "memory/details.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <queue>
#include <thread>
#include <utils/Time.h>

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

        [[maybe_unused]] int count = tmp.size();
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

    assert(memory::isAligned<uint64_t>(data));
    for (int i = 0; i < elementCount; ++i)
    {
        data[i].set(i + 0x400);
        list.push(&data[i]);
        if (i % 100 == 0)
        {
            utils::Time::uSleep(1000);
        }
    }

    const uint64_t ms = 1000;
    utils::Time::uSleep(10000 * ms);
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

TEST(LFList, consistencyPlenty)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    consistencyTest<LockFreeList>(8, 7 * BATCH_SIZE);
}
TEST(LFList, consistencyFew)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    consistencyTest<LockFreeList>(8, 128);
}

TEST(LFList, plainConsistencyPlenty)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    consistencyTest<QueueWrapper>(8, 7 * BATCH_SIZE);
}

struct TransmissionReport
{
    TransmissionReport() : ssrc(0), sent(0), received(0), disordered(0), fullHits(0), expSeqNo(0), sleepCount(0) {}

    int ssrc;
    std::atomic_int sent;
    std::atomic_int received;
    int disordered;
    std::atomic_int fullHits;
    int expSeqNo;
    int sleepCount;
};

bool throughputRunning = true;
template <typename QItem, typename Q>
void throughputRun(Q* queue, TransmissionReport* reports, bool validateSeqNo)
{
    int count = 0;
    int sleepCount = 0;
    std::vector<ListItem*> elements;
    elements.reserve(3000);
    uint32_t _pushFails = 0;

    for (; throughputRunning;)
    {
        ListItem* p;
        const auto loops = static_cast<size_t>(rand() + 100) % 3000;
        while (elements.size() < loops && queue->pop(p))
        {
            QItem* val = reinterpret_cast<QItem*>(p);
            ++reports[val->ssrc].received;
            ++count;
            if (validateSeqNo)
            {
                if (reports[val->ssrc].expSeqNo != val->seqNo)
                {
                    ++reports[val->ssrc].disordered;
                }
                reports[val->ssrc].expSeqNo = val->seqNo + 1;
            }

            elements.push_back(val);
        }
        for (auto& e : elements)
        {
            while (!queue->push(e))
            {
                _pushFails++;
                if (!throughputRunning)
                {
                    break;
                }
                std::this_thread::yield();
            }
        }
        elements.clear();

        ++sleepCount;
        utils::Time::nanoSleep(10);
    }

    for (ListItem* p = nullptr; queue->pop(p);)
    {
        ++count;
    }

    logger::info("got %d, slept %d, pushfails %u", "consumer", count, sleepCount, _pushFails);
}

struct ValueItem : public concurrency::ListItem
{
    uint32_t ssrc;
    int seqNo;
    uint64_t separator[7];
};

template <typename QItem, typename Q>
void throughputTest(Q& queue, const uint32_t threadCount, TransmissionReport* reports)
{
    throughputRunning = true;
    auto data = new QItem[3000 * threadCount];
    uint32_t ssrc = 0;
    for (uint32_t i = 0; i < 3000 * threadCount; ++i)
    {
        data[i].seqNo = 50 + i;
        data[i].ssrc = ssrc;
        if (i % 3000 == 2999)
        {
            ssrc++;
        }
        queue.push(&data[i]);
    }

    std::thread* prod[threadCount];
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        prod[i] = new std::thread(throughputRun<QItem, Q>, &queue, reports, false);
    }

    utils::Time::nanoSleep(4 * utils::Time::sec);
    throughputRunning = false;
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        prod[i]->join();
    }

    delete[] data;
}

TEST(LFList, throughput)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    LockFreeList queue;
    TransmissionReport reports[7];
    throughputTest<ValueItem>(queue, 7, reports);
}

TEST(LFList, throughputMQ)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    MpmcQueue<ListItem*> queue(3000 * 7 + 50003);

    TransmissionReport reports[7];
    throughputTest<ValueItem>(queue, 7, reports);
}
