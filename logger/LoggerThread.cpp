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
      _logFile(nullptr),
      _logStdOut(logStdOut),
      _logFileName(logFileName && std::strlen(logFileName) > 0 ? logFileName : ""),
      _droppedLogs(0),
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
        auto item = _logQueue.front();
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

/**
 * logLevel must be static eternal const string in memory.
 */
void LoggerThread::post(std::chrono::system_clock::time_point timestamp,
    const char* logLevel,
    const char* logGroup,
    void* threadId,
    const char* format,
    va_list args)
{
    va_list args2ndSprintf;

    const int maxMessageLength = 300;
    char smallMessage[maxMessageLength + 1];
    const int groupLength = snprintf(smallMessage, sizeof(smallMessage), "[%s] ", logGroup);
    if (groupLength < 0)
    {
        assert(false);
        return;
    }

    va_copy(args2ndSprintf, args);
    const int messageLength = vsnprintf(smallMessage + groupLength, sizeof(smallMessage) - groupLength, format, args);
    if (messageLength < 0)
    {
        assert(false);
        va_end(args2ndSprintf);
        return;
    }
    const int logLength = messageLength + groupLength;

    concurrency::ScopedAllocCommit<LogItem> memBlock(_logQueue, logLength + 1 + sizeof(LogItem));
    if (memBlock)
    {
        LogItem& log = *memBlock;
        log.logLevel = logLevel;
        log.threadId = threadId;
        log.timestamp = timestamp;
        if (logLength <= maxMessageLength)
        {
            std::strncpy(log.message, smallMessage, logLength + 1);
        }
        else
        {
            const int groupLength = snprintf(log.message, logLength + 1, "[%s] ", logGroup);
            const int messageLength =
                vsnprintf(log.message + groupLength, logLength + 1 - groupLength, format, args2ndSprintf);
            assert(messageLength + groupLength <= logLength);
        }
    }
    else
    {
        ++_droppedLogs;
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
    char message[maxMessageLength + 1];
    const int groupLength = snprintf(message, maxMessageLength, "[%s] ", logGroup);
    vsnprintf(message + groupLength, maxMessageLength - groupLength, format, args);

    if (_logStdOut)
    {
        formatTo(stdout, localTime, logLevel, threadId, message);
        fflush(stdout);
    }
    if (_logFile)
    {
        formatTo(_logFile, localTime, logLevel, threadId, message);
        fflush(_logFile);
    }
}

void LoggerThread::flush()
{
    LogItem item;
    while (!_logQueue.empty())
    {
        auto item = _logQueue.front();
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
