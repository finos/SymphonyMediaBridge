#include "Logger.h"
#include "LoggerThread.h"
#include "utils/Time.h"

namespace logger
{

Level _logLevel = Level::INFO;
std::atomic<size_t> LoggableId::_lastInstanceId;

std::unique_ptr<LoggerThread> _logThread;

void setup(const char* logFileName, bool logToStdOut, bool logToStdErr, Level level, size_t backlogSize)
{
    _logLevel = level;
    _logThread.reset(new LoggerThread(logFileName, logToStdOut, logToStdErr, backlogSize));
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

void disableStdOut() 
{
    if (_logThread) {
        _logThread->disableStdOut();
    }
}

void disableStdErr()
{
    if (_logThread) {
        _logThread->disableStdErr();
    }
}

void logv(const char* logLevel, const char* logGroup, const bool immediate, const char* format, va_list args)
{
    if (_logThread)
    {
        auto timestamp = utils::Time::now();
        auto threadId = (void*)pthread_self();

        if (immediate)
        {
            _logThread->immediate(timestamp, logLevel, logGroup, threadId, format, args);
        }
        else
        {
            _logThread->post(timestamp, logLevel, logGroup, threadId, format, args);
        }
    }
}

void logStackImmediate(void* const* stack, int frames, const char* logGroup)
{
    if (_logThread)
    {
        char** strings = backtrace_symbols(stack, frames);

        for (int i = 0; i < frames; ++i)
        {
            logger::errorImmediate("%s", logGroup, strings[i]);
        }
        free(strings);
    }
}

void logStack(void* const* stack, int frames, const char* logGroup)
{
    if (_logThread)
    {
        char** strings = backtrace_symbols(stack, frames);

        for (int i = 0; i < frames; ++i)
        {
            logger::warn("%s", logGroup, strings[i]);
        }
        free(strings);
    }
}

void flushLog()
{
    if (!_logThread)
    {
        return;
    }
    _logThread->awaitLogDrained(0, utils::Time::ms * 500);
}

void awaitLogDrained(float level, uint64_t timeoutNs)
{
    if (!_logThread)
    {
        return;
    }
    _logThread->awaitLogDrained(level, timeoutNs);
}

} // namespace logger
