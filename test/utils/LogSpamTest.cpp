#include "logger/Logger.h"
#include "logger/LoggerThread.h"
#include "logger/PruneSpam.h"
#include "logger/SuspendSpam.h"
#include <gtest/gtest.h>
#include <signal.h>
#include <unordered_map>

TEST(LogSpam, prune)
{
    uint32_t logCount = 0;

    logger::PruneSpam pruner(10, 50);
    for (int i = 0; i < 1001; ++i)
    {
        if (pruner.canLog())
        {
            ++logCount;
        }
    }

    EXPECT_EQ(logCount, 30);
}

TEST(LogSpam, suspend)
{
    uint32_t logCount = 0;

    logger::SuspendSpam pruner(10, utils::Time::sec * 2);
    for (uint64_t timestamp = 1; timestamp < utils::Time::sec * 61; timestamp += utils::Time::ms * 10)
    {
        if (pruner.canLog(timestamp))
        {
            ++logCount;
        }
    }

    EXPECT_EQ(logCount, 40);
}

namespace logger
{

extern std::unique_ptr<LoggerThread> _logThread;
} // namespace logger

TEST(LogSpam, logSizes)
{
    char log[1000];
    std::fill(log, log + sizeof(log) - 1, 'o');
    for (int i = 1; i < 990; ++i)
    {
        log[i] = '\0';
        logger::debug("%s", "", log);
        log[i] = 'o';
    }

    EXPECT_EQ(logger::_logThread->getDroppedLogCount(), 0);
}

TEST(Logger, flush)
{
    for (int i = 0; i < 500; ++i)
    {
        logger::info("test rather long log %d, test rather long log test rather long log test rather long log test "
                     "rather long log test rather long log test rather long log test rather long log ",
            "test",
            i);
    }

    logger::flushLog();
    EXPECT_TRUE(true);
}

#if 0 // Test will cause segfault. Useful for testing signals but should not be enabled by default

namespace
{
std::unordered_map<int32_t, struct sigaction> oldSignalHandlers;

void fatalSignalHandler(int32_t signalId)
{
    void* array[16];
    const auto size = backtrace(array, 16);
    logger::errorImmediate("Fatal signal %d, %d stack frames.", "fatalSignalHandler", signalId, size);
    logger::logStack(array, size, "fatalSignalHandler");
    logger::error("signalHandler %d", "fatalSignalHandler", signalId);

    logger::flushLog();

    if (signalId == SIGPIPE)
    {
        return;
    }

    auto oldSignalHandlersItr = oldSignalHandlers.find(signalId);
    if (oldSignalHandlersItr != oldSignalHandlers.end())
    {
        if (sigaction(signalId, &oldSignalHandlersItr->second, nullptr) != 0)
        {
            exit(signalId);
        }
    }
}
} // namespace

TEST(Logger, crash)
{
    struct sigaction sigactionData = {};
    sigactionData.sa_handler = fatalSignalHandler;
    sigactionData.sa_flags = 0;
    sigemptyset(&sigactionData.sa_mask);
    struct sigaction oldHandler = {};

    sigaction(SIGSEGV, &sigactionData, &oldHandler);
    oldSignalHandlers.emplace(SIGSEGV, oldHandler);

    for (int i = 0; i < 500; ++i)
    {
        logger::info("test rather long log %d, test rather long log test rather long log test rather long log test "
                     "rather long log test rather long log test rather long log test rather long log ",
            "test",
            i);
    }

    int* p = nullptr;
    auto h = p[9];
    (void)h; // silence warning
}
#endif
