#include "LoggerThread.h"
#include "concurrency/ThreadUtils.h"
#include "utils/Time.h"
#include <execinfo.h>
#include <filesystem>
#include <fstream>

namespace logger
{

const auto timeStringLength = 32;

/**
 * backlogSize in bytes must be 2^n
 */
LoggerThread::LoggerThread(const char* logFileName, bool logStdOut, size_t backlogSize)
    : _running(true),
      _logQueue(backlogSize),
      _logFile(nullptr),
      _logStdOut(logStdOut),
      _logFileName(logFileName && std::strlen(logFileName) > 0 ? logFileName : ""),
      _thread(new std::thread([this] { this->run(); }))
{
    _reOpenLog.test_and_set();
    reopenLogFile();
}

void LoggerThread::reopenLogFile()
{
    if (_logFile)
    {
        ::fclose(_logFile);
    }
    _logFile = _logFileName.size() ? fopen(_logFileName.c_str(), "a+") : nullptr;
}

namespace
{

inline void formatTo(FILE* fh, const char* localTime, const char* level, const void* threadId, const char* message)
{
    fprintf(fh, "%s %s [%p]%s\n", localTime, level, threadId, message);
}

} // namespace

void LoggerThread::run()
{
    concurrency::setThreadName("Logger");
    char localTime[timeStringLength];
    LogItem item;
    bool gotLogItem = false;
    for (;;)
    {
        auto item = _logQueue.front<LogItem>();
        if (item)
        {
            gotLogItem = true;
            formatTime(item->timestamp, localTime);

            if (_logStdOut)
            {
                formatTo(stdout, localTime, item->logLevel, item->threadId, item->message);
            }
            if (_logFile)
            {
                formatTo(_logFile, localTime, item->logLevel, item->threadId, item->message);
            }
            _logQueue.pop();
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
            if (!_reOpenLog.test_and_set())
            {
                reopenLogFile();
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

void LoggerThread::post(std::chrono::system_clock::time_point timestamp,
    const char* logLevel,
    const char* logGroup,
    void* threadId,
    const char* format,
    va_list args)
{
    va_list args2ndSprintf;
    va_copy(args2ndSprintf, args);

    const int maxMessageLength = 300;
    char mediumMessage[maxMessageLength + 1];
    const int consumed = snprintf(mediumMessage, maxMessageLength, "[%s] ", logGroup);
    const int remain = maxMessageLength - consumed;
    const int neededSpace = vsnprintf(mediumMessage + consumed, remain, format, args);
    const int totalSpace = neededSpace + consumed + 1;

    concurrency::ScopedAllocCommit item(_logQueue, totalSpace + sizeof(LogItem));
    if (item)
    {

        LogItem& log = item.get<LogItem>();
        log.logLevel = logLevel;
        log.threadId = threadId;
        log.timestamp = timestamp;
        if (neededSpace < maxMessageLength - consumed)
        {
            std::strncpy(log.message, mediumMessage, totalSpace);
        }
        else
        {
            const int consumed = snprintf(log.message, maxMessageLength, "[%s] ", logGroup);
            const int remain = totalSpace - consumed;
            const int neededSpace = vsnprintf(log.message + consumed, remain, format, args2ndSprintf);
            assert(neededSpace + consumed < totalSpace);
        }
    }
    va_end(args2ndSprintf);
}

void LoggerThread::immediate(std::chrono::system_clock::time_point timestamp,
    const char* logLevel,
    const char* logGroup,
    void* threadId,
    const char* format,
    va_list args)
{
    char localTime[timeStringLength];

    formatTime(timestamp, localTime);

    const size_t maxMessageLength = 4096;
    char mediumMessage[maxMessageLength + 1];
    const int consumed = snprintf(mediumMessage, maxMessageLength, "[%s] ", logGroup);
    const int remain = maxMessageLength - consumed;
    vsnprintf(mediumMessage + consumed, remain, format, args);

    if (_logStdOut)
    {
        formatTo(stdout, localTime, logLevel, threadId, mediumMessage);
        fflush(stdout);
    }
    if (_logFile)
    {
        formatTo(_logFile, localTime, logLevel, threadId, mediumMessage);
        fflush(_logFile);
    }
}

void LoggerThread::flush()
{
    LogItem item;
    while (!_logQueue.empty())
    {
        auto item = _logQueue.front<LogItem>();
        if (item)
        {
            char localTime[timeStringLength];
            formatTime(item->timestamp, localTime);

            if (_logStdOut)
            {
                formatTo(stdout, localTime, item->logLevel, item->threadId, item->message);
            }
            if (_logFile)
            {
                formatTo(_logFile, localTime, item->logLevel, item->threadId, item->message);
            }
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

void LoggerThread::formatTime(const std::chrono::system_clock::time_point timestamp, char* output)
{
    using namespace std::chrono;
    const std::time_t currentTime = system_clock::to_time_t(timestamp);
    tm currentLocalTime = {};
    localtime_r(&currentTime, &currentLocalTime);

    const auto ms = duration_cast<milliseconds>(timestamp.time_since_epoch()).count();

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
