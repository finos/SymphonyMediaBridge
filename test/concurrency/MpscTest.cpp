#include "TestValues.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include <gtest/gtest.h>
#include <inttypes.h>
#include <memory>
#include <queue>
#include <thread>
#include <utils/Time.h>

using namespace concurrency;

template <typename T, size_t S>
class LockFullMpscQueue
{
public:
    typedef T value_type;
    LockFullMpscQueue() {}

    bool pop(T& target)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.size() == 0)
        {
            return false;
        }
        target = _queue.front();
        _queue.pop();
        return true;
    }

    bool push(T&& obj)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.size() >= S)
        {
            return false;
        }
        _queue.push(obj);
        return true;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queue.size();
    }

    bool empty() const { return size() == 0; }

    bool full() const { return size() == S; }

private:
    std::queue<T> _queue;
    mutable std::mutex _mutex;
};

std::atomic<bool> producerRunning(true);
std::atomic<bool> consumerRunning(true);

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

template <typename Q>
void consumerRun(Q* queue, TransmissionReport reports[], bool validateSeqNo)
{
    int count = 0;
    int sleepCount = 0;
    for (;;)
    {
        while (!queue->empty())
        {
            typename Q::value_type val;
            if (queue->pop(val))
            {
                ++reports[val.ssrc].received;
                ++count;
                if (validateSeqNo)
                {
                    if (reports[val.ssrc].expSeqNo != val.seqNo)
                    {
                        ++reports[val.ssrc].disordered;
                    }
                    reports[val.ssrc].expSeqNo = val.seqNo + 1;
                }
            }
        }
        if (queue->empty() && !consumerRunning)
        {
            break;
        }

        ++sleepCount;
        utils::Time::nanoSleep(50);
    }

    logger::info("got %d, slept %d", "consumer", count, sleepCount);
}

template <typename Q>
void produceRun(uint32_t id, Q* queue, TransmissionReport reports[])
{
    int count = 0;
    while (producerRunning)
    {
        for (int i = 0; i < 3000; ++i)
        {
            if (queue->push(typename Q::value_type(id, count)))
            {
                ++reports[id].sent;
                ++count;
            }
            else
            {
                ++reports[id].fullHits;
                break;
            }
        }
        utils::Time::nanoSleep(10);
        ++reports[id].sleepCount;
    }
}

template <typename LockQueue>
void runQueueTest(const int consumerCount,
    const int producerCount,
    LockQueue& queue,
    TransmissionReport reports[],
    uint32_t durationMs)
{
    utils::Time::initialize();
    producerRunning = true;
    consumerRunning = true;
    std::unique_ptr<std::thread> prod[producerCount];
    for (int i = 0; i < producerCount; ++i)
    {
        prod[i] = std::make_unique<std::thread>(produceRun<LockQueue>, i, &queue, reports);
    }

    std::unique_ptr<std::thread> cons[consumerCount];
    for (int i = 0; i < consumerCount; ++i)
    {
        cons[i] = std::make_unique<std::thread>(consumerRun<LockQueue>, &queue, reports, consumerCount == 1);
    }

    utils::Time::usleep(durationMs * 1000ull);
    producerRunning = false;
    for (int i = 0; i < producerCount; ++i)
    {
        prod[i]->join();
    }

    consumerRunning = false;
    for (int i = 0; i < consumerCount; ++i)
    {
        cons[i]->join();
    }

    for (int i = 0; i < producerCount; ++i)
    {
        TransmissionReport& report = reports[i];
        if (report.sent > 0 || report.received > 0)
        {
            logger::info("%d sent %d, consumed %d, disorder %d, fullHits %d, slept %d",
                "producer",
                report.ssrc,
                report.sent.load(),
                report.received.load(),
                report.disordered,
                report.fullHits.load(),
                report.sleepCount);
        }
    }
}

const int PRODUCER_COUNT = 5;

TEST(Mpsc, DISABLED_mutexqueue)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new LockFullMpscQueue<Simple, 262144>();
    runQueueTest(1, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;

    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 300000);
            EXPECT_EQ(report.disordered, 0);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpsc, freequeue)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new MpmcQueue<Simple>(262144);
    runQueueTest(1, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;
    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 400000);
            EXPECT_EQ(report.disordered, 0);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpsc, freequeueSmall)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new MpmcQueue<SimpleSmall>(262144);
    runQueueTest(1, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;
    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 400000);
            EXPECT_EQ(report.disordered, 0);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpmc, DISABLED_mutexqueue)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new LockFullMpscQueue<Simple, 262144>();
    runQueueTest(3, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;

    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 300000);
            EXPECT_LT(report.fullHits, 130000);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpmc, freequeue)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new MpmcQueue<Simple>(262144 * 2);
    runQueueTest(3, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;
    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 400000);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpmc, freequeueSmall)
{
    TransmissionReport reports[PRODUCER_COUNT];
    auto queue = new MpmcQueue<SimpleSmall>(262144);
    runQueueTest(3, PRODUCER_COUNT, *queue, reports, 4000);
    delete queue;
    for (int i = 0; i < PRODUCER_COUNT; ++i)
    {
        TransmissionReport& report = reports[i];
        if (!__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer))
        {
            EXPECT_GT(report.received, 400000);
            EXPECT_EQ(report.sent, report.received);
        }
    }
}

TEST(Mpmc, FullEmptyCondition)
{
    const uint32_t SIZE = 1024;
    auto queue = std::make_unique<MpmcQueue<Simple>>(SIZE);
    for (uint32_t i = 0; i < SIZE; ++i)
    {
        EXPECT_TRUE(queue->push(Simple()));
        EXPECT_EQ(queue->size(), i + 1);
    }
    EXPECT_FALSE(queue->push(Simple()));
    EXPECT_EQ(queue->size(), SIZE);

    Simple item;
    for (uint32_t i = 0; i < SIZE - 1; ++i)
    {
        EXPECT_TRUE(queue->pop(item));
        EXPECT_FALSE(queue->empty());
        EXPECT_EQ(queue->size(), SIZE - i - 1);
    }
    EXPECT_TRUE(queue->pop(item));
    EXPECT_TRUE(queue->empty());

    EXPECT_FALSE(queue->pop(item));
}

namespace
{
class Counter
{
public:
    Counter(int& counter) : _counter(counter) { ++counter; }
    ~Counter() { --_counter; }

private:
    int& _counter;
};
} // namespace

TEST(Mpmc, uniqueptr)
{
    int counter = 0;
    auto queue = new MpmcQueue<std::unique_ptr<Counter>>(262144);
    for (int i = 0; i < 55000; ++i)
    {
        queue->push(std::make_unique<Counter>(counter));
    }

    EXPECT_EQ(counter, 55000);
    {
        std::unique_ptr<Counter> value;
        while (queue->pop(value)) {}
    }
    EXPECT_EQ(counter, 0);
    delete queue;
}
