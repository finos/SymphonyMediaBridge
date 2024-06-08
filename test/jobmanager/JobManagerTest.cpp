#include "jobmanager/JobManager.h"
#include "concurrency/Semaphore.h"
#include "jobmanager/JobQueue.h"
#include "jobmanager/WorkerThread.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>

using namespace std;
using namespace jobmanager;
using namespace concurrency;

namespace
{

const auto n = 1000000;
const int numWorkers = 4;

struct TestContext
{
    atomic_int32_t jobsCompleted;
    vector<size_t> jobCompleted;
    atomic_int32_t concurrency;
    atomic_bool maxConcurrencyReached;
    Semaphore allowedPendingJobs;
    TestContext()
        : jobsCompleted(0),
          jobCompleted(n),
          concurrency(0),
          maxConcurrencyReached(false),
          allowedPendingJobs(1024)
    {
    }
};

struct TestJob : public Job
{
    TestJob(TestContext& context, useconds_t delay, int jobIndex, atomic_int32_t* serialConcurrency)
        : context(context),
          delay(delay),
          jobIndex(jobIndex),
          serialConcurrency(serialConcurrency)
    {
    }

    void run() override
    {
        const auto currConcurrency = ++context.concurrency;
        EXPECT_LE(currConcurrency, numWorkers);
        if (serialConcurrency)
        {
            const auto currSerialConcurrency = ++*serialConcurrency;
            assert(currSerialConcurrency == 1);
            EXPECT_EQ(currSerialConcurrency, 1);
        }
        if (currConcurrency == numWorkers)
        {
            context.maxConcurrencyReached.store(true);
        }
        usleep(delay);
        context.jobCompleted[jobIndex] = true;
        ++context.jobsCompleted;
        context.allowedPendingJobs.post();
        --context.concurrency;
        if (serialConcurrency)
        {
            --*serialConcurrency;
        }
    }

    TestContext& context;
    const useconds_t delay;
    const int jobIndex;
    atomic_int32_t* serialConcurrency;
};

template <class JOB_MANAGER>
void writer(TestContext& ctx,
    int beginJobIndex,
    int endJobIndex,
    JOB_MANAGER& jobManager,
    atomic_int32_t* serialConcurrency)
{
    default_random_engine generator(n);
    uniform_int_distribution<useconds_t> queueingDistribution(0, 10);
    uniform_int_distribution<useconds_t> jobRuntimeDistribution(0, 5);

    for (auto i = beginJobIndex; i < endJobIndex; ++i)
    {
        ctx.allowedPendingJobs.wait();
        jobManager.template addJob<TestJob>(ctx, jobRuntimeDistribution(generator), i, serialConcurrency);
        usleep(queueingDistribution(generator));
    }
}

} // namespace

struct JobManagerTest : public ::testing::Test
{
    jobmanager::TimerQueue timers;
    JobManager jobManager;
    vector<unique_ptr<WorkerThread>> workerThreads;
    TestContext context;

    JobManagerTest() : timers(512), jobManager(timers), context() {}

    void SetUp() override
    {
        for (int i = 0; i < numWorkers; ++i)
        {
            workerThreads.emplace_back(make_unique<WorkerThread>(jobManager, true));
        }
    }

    void TearDown() override
    {
        jobManager.stop();
        for (auto i = workerThreads.begin(); i != workerThreads.end(); ++i)
        {
            (*i)->stop();
        }
    }
};

TEST_F(JobManagerTest, concurrentJobs)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif

    thread writerThread1(writer<JobManager>, ref(context), 0, n / 2, ref(jobManager), nullptr);
    thread writerThread2(writer<JobManager>, ref(context), n / 2, n, ref(jobManager), nullptr);
    writerThread1.join();
    writerThread2.join();

    for (int iter = 0; context.jobsCompleted.load() != n; ++iter)
    {
        EXPECT_LT(iter, 100);
        usleep(10000UL);
    }

    for (int i = 0; i < n; ++i)
    {
        EXPECT_TRUE(context.jobCompleted[i]);
    }

    EXPECT_TRUE(context.maxConcurrencyReached.load());
}

TEST_F(JobManagerTest, serialJobs)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif

    atomic_int32_t serialConcurrency1(0);
    atomic_int32_t serialConcurrency2(0);

    JobQueue serialJobs1(jobManager);
    JobQueue serialJobs2(jobManager);

    thread writerThread1(writer<JobQueue>, ref(context), 0, n / 2, ref(serialJobs1), &serialConcurrency1);
    thread writerThread2(writer<JobQueue>, ref(context), n / 2, n, ref(serialJobs2), &serialConcurrency2);
    writerThread1.join();
    writerThread2.join();

    for (int iter = 0; context.jobsCompleted.load() != n; ++iter)
    {
        EXPECT_LT(iter, 150);
        usleep(10000UL);
    }

    for (int i = 0; i < n; ++i)
    {
        EXPECT_TRUE(context.jobCompleted[i]);
    }
}

class NoJob : public Job
{
public:
    explicit NoJob(std::atomic_int& counter) : decremented(false), _counter(counter) { ++counter; }
    ~NoJob()
    {
        if (!decremented)
        {
            --_counter;
        }
    }
    void run() override
    {
        --_counter;
        decremented = true;
    }

    bool decremented;

private:
    std::atomic_int& _counter;
};

class BlockingJob : public Job
{
public:
    BlockingJob(Semaphore& sem) : _sem(sem) {}
    void run() { _sem.wait(); }

private:
    Semaphore& _sem;
};

TEST_F(JobManagerTest, serialJobQueueFull)
{
    JobQueue serialJobQ(jobManager, 32);

    std::atomic_int mainCounter(0);
    std::atomic_int counter(0);
    Semaphore sem;
    for (int i = 0; i < numWorkers; ++i)
    {
        jobManager.template addJob<BlockingJob>(sem);
    }

    while (jobManager.template addJob<NoJob>(mainCounter))
        ;
    utils::Time::uSleep(15000);
    EXPECT_GT(mainCounter.load(), 4096);
    for (int i = 0; i < 10; ++i)
    {
        if (!serialJobQ.addJob<NoJob>(counter))
        {
            break;
        }
    }

    utils::Time::uSleep(5000);
    EXPECT_EQ(counter.load(), 10);

    while (serialJobQ.addJob<NoJob>(counter))
        ;

    auto full = counter.load();
    EXPECT_GT(full, 30);
    for (int i = 0; i < numWorkers * 10; ++i)
    {
        sem.post();
    }
    utils::Time::uSleep(500000);
    EXPECT_EQ(mainCounter.load(), 0);
    EXPECT_EQ(counter.load(), full);

    serialJobQ.addJob<NoJob>(counter);
    utils::Time::uSleep(500000);

    EXPECT_EQ(counter.load(), 0);
}

class TimerJob : public Job
{
public:
    TimerJob(concurrency::Semaphore& sem) : _sem(sem) {}

    void run() override { _sem.post(); }

private:
    concurrency::Semaphore& _sem;
};

TEST_F(JobManagerTest, timers)
{
    concurrency::Semaphore sem0(0);
    concurrency::Semaphore sem1(0);
    concurrency::Semaphore sem2(0);
    const uint64_t ms = 1000000;
    const uint64_t timeout = 500 * ms;
    auto start = utils::Time::getAbsoluteTime();
    jobManager.template addTimedJob<TimerJob>(3, 4, timeout * 3 / 1000, sem2);
    jobManager.template addTimedJob<TimerJob>(1, 4, timeout / 1000, sem0);
    jobManager.template addTimedJob<TimerJob>(2, 4, timeout * 2 / 1000, sem1);
    sem0.wait();
    auto end = utils::Time::getAbsoluteTime();
    EXPECT_NEAR(end - start, timeout, 30 * ms);
    sem1.wait();
    end = utils::Time::getAbsoluteTime();
    EXPECT_NEAR(end - start, timeout * 2, 30 * ms);
    sem2.wait();
    end = utils::Time::getAbsoluteTime();
    EXPECT_NEAR(end - start, timeout * 3, 30 * ms);
}

namespace
{
class YieldJob : public MultiStepJob
{
public:
    YieldJob(std::atomic_int& counter) : _start(utils::Time::getAbsoluteTime()), _counter(counter) { ++counter; }

    bool runStep() override
    {
        if (utils::Time::diffLT(_start, utils::Time::getAbsoluteTime(), utils::Time::sec))
        {
            return true;
        }
        --_counter;
        return false;
    }

private:
    uint64_t _start;
    std::atomic_int& _counter;
};
} // namespace

TEST_F(JobManagerTest, yieldingJobs)
{
    std::atomic_int counter(0);
    std::atomic_int yieldJobCounter(0);
    for (int i = 0; i < 600; ++i)
    {
        jobManager.addJob<YieldJob>(yieldJobCounter);
        jobManager.addJob<NoJob>(counter);
        utils::Time::nanoSleep(utils::Time::ms);
    }

    while (jobManager.getCount() > 0)
    {
        utils::Time::nanoSleep(utils::Time::ms);
    }

    utils::Time::nanoSleep(utils::Time::ms * 1050);

    EXPECT_EQ(yieldJobCounter.load(), 0);
    EXPECT_EQ(counter.load(), 0);
}

class QueueDeleteJob : public jobmanager::Job
{
public:
    QueueDeleteJob(jobmanager::JobQueue* queue) : _queue(queue) {}
    void run() override
    {
        logger::info("deleting queue", "QueueDeleteJob");
        delete _queue;
        logger::info("deleted queue", "QueueDeleteJob");
    }

private:
    jobmanager::JobQueue* _queue;
};

class SleepJob : public jobmanager::Job
{
public:
    SleepJob(uint64_t timeout) : _timeout(timeout) {}

    void run() override
    {
        logger::info("start sleep ", "SleepJob");
        utils::Time::nanoSleep(_timeout);
        logger::info("slept ", "SleepJob");
    }

private:
    uint64_t _timeout;
};

// this test relies on workerthread yield.
TEST_F(JobManagerTest, jobQueueDeletion)
{
    jobmanager::JobQueue* owners[numWorkers];
    for (int i = 0; i < numWorkers; ++i)
    {
        owners[i] = new jobmanager::JobQueue(jobManager);
    }

    // make all workers busy
    for (int i = 0; i < numWorkers; ++i)
    {
        jobManager.addJob<SleepJob>(utils::Time::ms * 5);
    }

    // place delete jobs for each worker
    for (int i = 0; i < numWorkers; ++i)
    {
        jobManager.addJob<QueueDeleteJob>(owners[i]);
    }

    // post on job queues so they cannot be deleted until jobs have been processed
    for (int i = 0; i < numWorkers; ++i)
    {
        owners[i]->addJob<SleepJob>(utils::Time::ms * 1);
        owners[i]->addJob<SleepJob>(utils::Time::ms * 1);
    }

    // this test will wait indefinitely if the workerthreads are not able to yield
    // in the ~JobQueue and process the remaining queued jobs.
    utils::Time::nanoSleep(utils::Time::ms * 30);
}
