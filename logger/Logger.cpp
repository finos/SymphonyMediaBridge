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

void logStack(const void* stack, int frames, const char* logGroup)
{
    if (_logThread)
    {
        void* array[16];
        const auto size = backtrace(array, 16);
        char** strings = backtrace_symbols(array, size);

        for (auto i = 0; i < size; ++i)
        {
            logger::debug("%s", "STACK", strings[i]);
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
    _logThread->flush();
}

void awaitLogDrained(float level)
{
    _logThread->awaitLogDrained(level);
}

} // namespace logger
