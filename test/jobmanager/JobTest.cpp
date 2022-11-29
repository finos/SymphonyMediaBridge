#include "jobmanager/Job.h"
#include "mocks/TimeSourceMock.h"
#include <gtest/gtest.h>

using namespace ::testing;

namespace
{

struct MultiStepWithTimeoutJobChildMock : public jobmanager::MultiStepWithTimeoutJob
{
    using jobmanager::MultiStepWithTimeoutJob::MultiStepWithTimeoutJob;

    MOCK_METHOD(void, onTimeout, ());
    MOCK_METHOD(bool, runTick, ());

    uint64_t getTimeLimit() const { return _timeLimit; }
};

} // namespace

class JobTest : public ::testing::Test
{

protected:
    void SetUp() override
    {
        _timeSourceMock = std::make_unique<NiceMock<test::TimeSourceMock>>();
        utils::Time::initialize(*_timeSourceMock);
    }

    void TearDown() override { utils::Time::initialize(); }

protected:
    std::unique_ptr<test::TimeSourceMock> _timeSourceMock;
};

TEST_F(JobTest, multiStepWithTimeoutInitialize)
{
    const uint64_t initialTime = 63473;
    const uint64_t timeout = 5 * utils::Time::sec;
    EXPECT_CALL(*_timeSourceMock, getAbsoluteTime()).WillOnce(Return(initialTime));

    StrictMock<MultiStepWithTimeoutJobChildMock> job(timeout);

    EXPECT_CALL(job, onTimeout()).Times(0);
    EXPECT_CALL(job, runTick()).Times(0);

    ASSERT_EQ(initialTime + timeout, job.getTimeLimit());
}

TEST_F(JobTest, multiStepWithTimeoutShouldNotContinueWhenTickReturnsFalse)
{
    const uint64_t initialTime = 63473;
    const uint64_t timeout = 5 * utils::Time::sec;
    EXPECT_CALL(*_timeSourceMock, getAbsoluteTime()).WillOnce(Return(initialTime));

    StrictMock<MultiStepWithTimeoutJobChildMock> job(timeout);

    EXPECT_CALL(job, runTick()).WillOnce(Return(false));

    const bool shouldRunAgain = job.runStep();
    ASSERT_EQ(false, shouldRunAgain);
}

TEST_F(JobTest, multiStepWithTimeoutShouldRunOnce)
{
    const uint64_t initialTime = 63473;
    const uint64_t timeout = 5 * utils::Time::sec;
    EXPECT_CALL(*_timeSourceMock, getAbsoluteTime())
        .WillOnce(Return(initialTime)) // First to create the job
        .WillOnce(Return(initialTime + timeout * 2)); // Fist time job run is already timed out

    StrictMock<MultiStepWithTimeoutJobChildMock> job(timeout);

    {
        InSequence seq;

        EXPECT_CALL(job, runTick()).WillOnce(Return(true));
        EXPECT_CALL(job, onTimeout());
    }

    const bool shouldRunAgain = job.runStep();
    ASSERT_EQ(false, shouldRunAgain);
}

TEST_F(JobTest, multiStepWithTimeoutShouldContinueUntilTimeout)
{
    const uint64_t initialTime = 63473;
    const uint64_t timeout = 5 * utils::Time::sec;
    EXPECT_CALL(*_timeSourceMock, getAbsoluteTime())
        .WillOnce(Return(initialTime))
        .WillOnce(Return(initialTime + 1 * 500 * utils::Time::ms))
        .WillOnce(Return(initialTime + 2 * 500 * utils::Time::ms))
        .WillOnce(Return(initialTime + 3 * 500 * utils::Time::ms))
        .WillOnce(Return(initialTime + 4 * 500 * utils::Time::ms))
        .WillOnce(Return(initialTime + 5 * 500 * utils::Time::ms))
        .WillOnce(Return(initialTime + timeout));

    StrictMock<MultiStepWithTimeoutJobChildMock> job(timeout);

    ON_CALL(job, runTick()).WillByDefault(Return(true));

    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_CALL(job, runTick());
        EXPECT_CALL(job, onTimeout()).Times(0);
        const bool shouldRunAgain = job.runStep();
        ASSERT_EQ(true, shouldRunAgain);
        Mock::VerifyAndClearExpectations(&job);
    }

    {
        InSequence seq;

        EXPECT_CALL(job, runTick());
        EXPECT_CALL(job, onTimeout());
    }

    // Job will timeout on the next call
    const bool shouldRunAgain = job.runStep();
    ASSERT_EQ(false, shouldRunAgain);
}