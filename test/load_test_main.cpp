#include "logger/Logger.h"
#include "test/integration/LoadTestConfig.h"
#include "utils/Time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>

using namespace ::testing;
class GtestMain : public ::testing::Environment
{
public:
    void SetUp() override
    {
        utils::Time::initialize();
        auto fh = fopen("./smb_load_test.log", "w");
        fclose(fh);
        logger::setup("./smb_load_test.log", true, true, logger::Level::DBG, 4 * 1024 * 1024);
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
        logger::info("Test Ended %s.%s (%" PRIi64 " ms) <<<",
            "gtest",
            test_info.test_case_name(),
            test_info.name(),
            test_info.result()->elapsed_time());
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

void initLoadTestConfig(const int argc, char** argv)
{
    static const char configPrefix[] = "--load_test_config=";
    config::g_LoadTestConfigFile = nullptr;
    for (auto i = 0; i < argc; i++)
    {
        const std::string arg = std::string(argv[i]);
        if (arg.length() > strlen(configPrefix) && arg.find(configPrefix) == 0)
        {
            config::g_LoadTestConfigFile = argv[i] + strlen(configPrefix);
            break;
        }
    }
}

int main(int argc, char** argv)
{
    ::testing::AddGlobalTestEnvironment(new GtestMain()); // Gtest takes ownership

    ::testing::InitGoogleTest(&argc, argv);

    initLoadTestConfig(argc, argv);

    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // Adds a listener to the end.  googletest takes the ownership.
    listeners.Append(new TestEventSink);

    return RUN_ALL_TESTS();
}
