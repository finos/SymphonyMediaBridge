#include "Logger.h"
#include "LoggerThread.h"
#include "utils/Time.h"

namespace logger
{

Level _logLevel = Level::INFO;
std::atomic<size_t> LoggableId::_lastInstanceId;

std::unique_ptr<LoggerThread> _logThread;

void setup(const char* logFileName, bool logToStdOut, Level level, size_t backlogSize)
{
    _logLevel = level;
    _logThread.reset(new LoggerThread(logFileName, logToStdOut, backlogSize));
}

void reOpenLog()
{
    if (_logThread)
    {
        _logThread->reopen();
    }
}

void stop()
{
    if (_logThread)
    {
        _logThread->stop();
        _logThread.reset();
    }
}

void logv(const char* logLevel, const char* logGroup, const bool immediate, const char* format, va_list args)
{
    if (_logThread)
    {
        LogItem item;
        item.timestamp = utils::Time::now();
        item.logLevel = logLevel;
        item.threadId = (void*)pthread_self();
        int consumed = snprintf(item.message, logger::MAX_LINE_LENGTH, "[%s] ", logGroup);
        int remain = logger::MAX_LINE_LENGTH - consumed;
        vsnprintf(item.message + consumed, remain, format, args);

        if (immediate)
        {
            _logThread->immediate(std::move(item));
        }
        else
        {
            _logThread->post(std::move(item));
        }
    }
}

void logStack(const void* stack, int frames, const char* logGroup)
{
    if (_logThread)
    {
        LogItem item;
        item.timestamp = utils::Time::now();
        item.logLevel = "_STK_";
        item.threadId = (void*)pthread_self();
        const int byteCount = sizeof(void*) * frames;
        std::memcpy(item.message, stack, byteCount);
        std::memset(item.message + byteCount, 0, sizeof(void*));
        std::strcpy(item.message + byteCount + sizeof(void*), logGroup);
        _logThread->immediate(std::move(item));
    }
}

void flushLog()
{
    if (!_logThread)
    {
        return;
    }
    _logThread->flush();
}

void awaitLogDrained(float level)
{
    _logThread->awaitLogDrained(level);
}

} // namespace logger
