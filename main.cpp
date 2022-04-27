#include "bridge/Bridge.h"
#include "concurrency/Semaphore.h"
#include "concurrency/ThreadUtils.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "utils/Time.h"
#include <execinfo.h>
#include <iostream>
#include <memory>
#include <signal.h>

namespace
{

std::unique_ptr<concurrency::Semaphore> running;
std::unordered_map<int32_t, struct sigaction> oldSignalHandlers;

void fatalSignalHandler(int32_t signalId)
{
    void* array[16];
    const auto size = backtrace(array, 16);
    char** strings = backtrace_symbols(array, size);
    logger::errorImmediate("Fatal signal %d, %d stack frames.", "fatalSignalHandler", signalId, size);

    for (auto i = 0; i < size; ++i)
    {
        logger::errorImmediate("%s", "fatalSignalHandler", strings[i]);
    }
    free(strings);

    logger::flushLog();

    auto oldSignalHandlersItr = oldSignalHandlers.find(signalId);
    if (oldSignalHandlersItr != oldSignalHandlers.end())
    {
        if (sigaction(signalId, &oldSignalHandlersItr->second, nullptr) != 0)
        {
            exit(signalId);
        }
    }
}

void intSignalHandler(int32_t)
{
    logger::info("SIGINT", "main");
    assert(running);
    running->post();
}

} // namespace

static bool isEqualCaseInsensitive(const std::string& s1, const std::string& s2)
{
    if (s1.size() != s2.size())
    {
        return false;
    }
    return std::equal(s1.begin(), s1.end(), s2.begin(), [](char a, char b) { return tolower(a) == tolower(b); });
}

static logger::Level parseLogLevel(const std::string& level)
{
    if (isEqualCaseInsensitive(level, "error"))
    {
        return logger::Level::ERROR;
    }
    if (isEqualCaseInsensitive(level, "warn") || isEqualCaseInsensitive(level, "warning"))
    {
        return logger::Level::WARN;
    }
    if (isEqualCaseInsensitive(level, "debug") || isEqualCaseInsensitive(level, "dbg"))
    {
        return logger::Level::DBG;
    }

    return logger::Level::INFO;
}

int main(int argc, char** argv)
{
    running = std::make_unique<concurrency::Semaphore>();
    concurrency::setThreadName("main");

    {
        struct sigaction sigactionData = {};
        sigactionData.sa_handler = intSignalHandler;
        sigactionData.sa_flags = 0;
        sigemptyset(&sigactionData.sa_mask);
        sigaction(SIGINT, &sigactionData, nullptr);
    }

    {
        struct sigaction sigactionData = {};
        sigactionData.sa_handler = fatalSignalHandler;
        sigactionData.sa_flags = 0;
        sigemptyset(&sigactionData.sa_mask);
        struct sigaction oldHandler = {};

        sigaction(SIGPIPE, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGPIPE, oldHandler);

        sigaction(SIGSEGV, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGSEGV, oldHandler);

        sigaction(SIGBUS, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGBUS, oldHandler);

        sigaction(SIGABRT, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGABRT, oldHandler);

        sigaction(SIGILL, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGILL, oldHandler);

        sigaction(SIGFPE, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGFPE, oldHandler);

        sigaction(SIGSYS, &sigactionData, &oldHandler);
        oldSignalHandlers.emplace(SIGSYS, oldHandler);
    }

    if (argc < 2)
    {
        std::cout << "Symphony RTC Mixer" << std::endl;
        std::cout << "Usage: smb config.json" << std::endl;
        return 0;
    }

    const auto config = std::make_unique<config::Config>();
    if (!config->readFromFile(argv[1]))
    {
        return 1;
    }

    utils::Time::initialize();
    logger::setup(config->logFile.get().c_str(), config->logStdOut, parseLogLevel(config->logLevel));
    logger::info("Starting httpd on port %u", "main", config->port.get());
    logger::info("Configured udp port range: %s  %u - %u",
        "main",
        config->ip.get().c_str(),
        config->ice.udpPortRangeLow.get(),
        config->ice.udpPortRangeHigh.get());
    logger::logAlways("log level %s", "main", config->logLevel.get().c_str());

    {
        bridge::Bridge environment(*config);
        environment.initialize();

        logger::info("Build: %s",
            "main",
#if DEBUG
            "Debug"
#else
            "Release"
#endif
        );

        if (environment.isInitialized())
        {
            running->wait();
        }
    }

    logger::stop();
    return 0;
}
