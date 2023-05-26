#include "logger/Logger.h"
#include "utils/Time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace ::testing;
class GtestMain : public ::testing::Environment
{
public:
    void SetUp() override
    {
        utils::Time::initialize();
        auto fh = fopen("./smb_unit_test2.log", "w");
        fclose(fh);
        logger::setup("./smb_unit_test2.log", true, logger::Level::DBG, 8 * 1024 * 1024);
    }

    void TearDown() override { logger::stop(); }
};

class TestEventSink : public ::testing::TestEventListener
{
public:
    virtual void OnTestProgramStart(const ::testing::UnitTest& unit_test) override {}
    virtual void OnTestIterationStart(const ::testing::UnitTest& unit_test, int iteration) override {}
    virtual void OnEnvironmentsSetUpStart(const UnitTest& unit_test) override {}
    virtual void OnEnvironmentsSetUpEnd(const UnitTest& unit_test) override {}
    virtual void OnTestStart(const TestInfo& test_info) override
    {
        utils::Time::initialize();
        logger::info(">>> Starting test %s.%s", "gtest", test_info.test_case_name(), test_info.name());
        srand(0xfb3b61a);
    }
    virtual void OnTestEnd(const TestInfo& test_info) override
    {
        utils::Time::initialize(); // the time source may be deleted by now
        logger::info("Test Ended %s.%s <<<", "gtest", test_info.test_case_name(), test_info.name());
        logger::awaitLogDrained();
    }
    virtual void OnEnvironmentsTearDownStart(const UnitTest& unit_test) override {}
    virtual void OnEnvironmentsTearDownEnd(const UnitTest& unit_test) override {}
    virtual void OnTestIterationEnd(const UnitTest& unit_test, int iteration) override {}
    virtual void OnTestProgramEnd(const UnitTest& unit_test) override {}
    virtual void OnTestPartResult(const TestPartResult& test_part_result) override
    {
        if (test_part_result.failed())
        {
            logger::error("Test Failure at %s:%d\n '%s'",
                "gtest",
                test_part_result.file_name(),
                test_part_result.line_number(),
                test_part_result.summary());
        }
    }
};

int main(int argc, char** argv)
{
    ::testing::AddGlobalTestEnvironment(new GtestMain()); // Gtest takes ownership

    ::testing::InitGoogleTest(&argc, argv);

    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // Adds a listener to the end.  googletest takes the ownership.
    listeners.Append(new TestEventSink);

    return RUN_ALL_TESTS();
}
