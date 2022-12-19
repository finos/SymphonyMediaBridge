#include "LoggerThread.h"
#include "concurrency/ThreadUtils.h"
#include "utils/Time.h"
#include <execinfo.h>
#include <filesystem>
#include <fstream>

namespace logger
{

const auto timeStringLength = 32;

LoggerThread::LoggerThread(const char* logFileName, bool logStdOut, size_t backlogSize)
    : _running(true),
      _logQueue(backlogSize),
      _logStdOut(logStdOut),
      _logFileName(logFileName && strlen(logFileName) > 0 ? logFileName : ""),
      _thread(new std::thread([this] { this->run(); }))
{
    reopenLogFile();
}

void LoggerThread::reopenLogFile()
{
    _logFile = _logFileName.size() ? fopen(_logFileName.c_str(), "a+") : nullptr;
}

namespace
{
inline void formatTo(FILE* fh,
    const char* localTime,
    const char* level,
    const void* threadId,
    const char* logGroup,
    const char* message)
{
    fprintf(fh, "%s %s [%p][%s] %s\n", localTime, level, threadId, logGroup, message);
}

inline void formatTo(FILE* fh, const char* localTime, const char* level, const void* threadId, const char* message)
{
    fprintf(fh, "%s %s [%p]%s\n", localTime, level, threadId, message);
}

void logStack(const LogItem& item, const char* localTime, bool logStdOut, FILE* logFile)
{
    int frames = 0;
    auto stack = reinterpret_cast<void**>(const_cast<LogItem&>(item).message);
    for (frames = 0; stack[frames] != nullptr; ++frames) {}
    auto logGroup = reinterpret_cast<const char*>(&stack[frames + 1]);
    char** strs = backtrace_symbols(stack, frames);

    for (int i = 0; i < frames; ++i)
    {
        if (logStdOut)
        {
            formatTo(stdout, localTime, "STACK", item.threadId, logGroup, strs[i]);
        }
        if (logFile)
        {
            formatTo(logFile, localTime, "STACK", item.threadId, logGroup, strs[i]);
        }
    }
    free(strs);
}
} // namespace

void LoggerThread::ensureLogFileExists()
{
    if (_logFileName.size())
    {
#if 0
        std::ifstream f(_logFileName.c_str());
        if (!f.good())
        {
            reopenLogFile();
        }
#else
        if (!std::filesystem::exists(_logFileName.c_str()))
        {
            reopenLogFile();
        }
#endif
    }
}

void LoggerThread::run()
{
    concurrency::setThreadName("Logger");
    char localTime[timeStringLength];
    LogItem item;
    bool gotLogItem = false;
    for (;;)
    {
        ensureLogFileExists();

        if (_logQueue.pop(item))
        {
            gotLogItem = true;
            formatTime(item, localTime);
#ifdef DEBUG
            if (0 == std::strcmp(item.logLevel, "_STK_"))
            {
                logStack(item, localTime, _logStdOut, _logFile);
                continue;
            }
#endif
            if (_logStdOut)
            {
                formatTo(stdout, localTime, item.logLevel, item.threadId, item.message);
            }
            if (_logFile)
            {
                formatTo(_logFile, localTime, item.logLevel, item.threadId, item.message);
            }
        }
        else
        {
            if (gotLogItem && _logStdOut)
            {
                fflush(stdout);
            }
            if (gotLogItem && _logFile)
            {
                fflush(_logFile);
            }
            gotLogItem = false;

            if (!_running.load(std::memory_order::memory_order_relaxed))
            {
                break;
            }
            utils::Time::rawNanoSleep(50 * utils::Time::ms);
        }
    }

    if (_logFile)
    {
        fclose(_logFile);
        _logFile = nullptr;
    }
}

void LoggerThread::immediate(const LogItem& item)
{
    char localTime[timeStringLength];

    formatTime(item, localTime);
#ifdef DEBUG
    if (0 == std::strcmp(item.logLevel, "_STK_"))
    {
        logStack(item, localTime, _logStdOut, _logFile);
        fflush(stdout);
        return;
    }
#endif
    if (_logStdOut)
    {
        formatTo(stdout, localTime, item.logLevel, item.threadId, item.message);
        fflush(stdout);
    }
    if (_logFile)
    {
        formatTo(_logFile, localTime, item.logLevel, item.threadId, item.message);
        fflush(_logFile);
    }
}

void LoggerThread::flush()
{
    LogItem item;
    while (_logQueue.pop(item))
    {
        char localTime[timeStringLength];
        formatTime(item, localTime);

        if (_logStdOut)
        {
            formatTo(stdout, localTime, item.logLevel, item.threadId, item.message);
        }
        if (_logFile)
        {
            formatTo(_logFile, localTime, item.logLevel, item.threadId, item.message);
        }
    }

    if (_logStdOut)
    {
        fflush(stdout);
    }
    if (_logFile)
    {
        fflush(_logFile);
    }
}

void LoggerThread::stop()
{
    _running = false;
    if (_thread)
    {
        _thread->join();
    }
}

void LoggerThread::formatTime(const LogItem& item, char* output)
{
    using namespace std::chrono;
    const std::time_t currentTime = system_clock::to_time_t(item.timestamp);
    tm currentLocalTime = {};
    localtime_r(&currentTime, &currentLocalTime);

    const auto ms = duration_cast<milliseconds>(item.timestamp.time_since_epoch()).count();

    snprintf(output,
        timeStringLength,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        currentLocalTime.tm_year + 1900,
        currentLocalTime.tm_mon + 1,
        currentLocalTime.tm_mday,
        currentLocalTime.tm_hour,
        currentLocalTime.tm_min,
        currentLocalTime.tm_sec,
        static_cast<int>(ms % 1000));
}

void LoggerThread::awaitLogDrained(float level)
{
    level = std::max(0.0f, std::min(1.0f, level));
    if (_logQueue.size() <= _logQueue.capacity() * level)
    {
        return;
    }

    while (!_logQueue.empty())
    {
        utils::Time::rawNanoSleep(100000);
    }
}

} // namespace logger
