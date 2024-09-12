#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <execinfo.h>
#include <inttypes.h> // for logging int64 with PRId64
namespace logger
{
enum class Level
{
    ERROR,
    WARN,
    INFO,
    DBG
};

extern Level _logLevel;
void setup(const char* logToFile, bool logToStdOut, bool logToStdErr, Level level, size_t backlogSize = 4096);
void stop();
void reOpenLog();

void logv(const char* logLevel, const char* logGroup, const bool immediate, const char* format, va_list args);
void logv(const char* logLevel, const char* logGroup, const char* format, va_list args);
void logStack(void* const* stack, int frames, const char* logGroup);
void logStackImmediate(void* const* stack, int frames, const char* logGroup);
void flushLog();
void awaitLogDrained(float level = 0.0f, uint64_t timeoutNs = 2000'000'000);

__attribute__((format(printf, 1, 3))) inline void info(const char* format, const char* logGroup, ...)
{
    if (logger::_logLevel < Level::INFO)
    {
        return;
    }

    va_list arglist;
    va_start(arglist, logGroup);
    logv("INFO", logGroup, false, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void warn(const char* format, const char* logGroup, ...)
{
    if (_logLevel < Level::WARN)
    {
        return;
    }

    va_list arglist;
    va_start(arglist, logGroup);
    logv("WARN", logGroup, false, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void error(const char* format, const char* logGroup, ...)
{
    va_list arglist;
    va_start(arglist, logGroup);
    logv("ERROR", logGroup, false, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void debug(const char* format, const char* logGroup, ...)
{
    if (logger::_logLevel < Level::DBG)
    {
        return;
    }

    va_list arglist;
    va_start(arglist, logGroup);
    logv("DEBUG", logGroup, false, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void logAlways(const char* format, const char* logGroup, ...)
{
    va_list arglist;
    va_start(arglist, logGroup);
    logv("ALL", logGroup, false, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void warnImmediate(const char* format, const char* logGroup, ...)
{
    va_list arglist;
    va_start(arglist, logGroup);
    logv("WARN", logGroup, true, format, arglist);
    va_end(arglist);
}

__attribute__((format(printf, 1, 3))) inline void errorImmediate(const char* format, const char* logGroup, ...)
{
    va_list arglist;
    va_start(arglist, logGroup);
    logv("ERROR", logGroup, true, format, arglist);
    va_end(arglist);
}

void disableStdOut();
void disableStdErr();

inline void logStack(const char* logGroup)
{
#ifdef DEBUG
    if (logger::_logLevel < Level::DBG)
    {
        return;
    }

    void* callstack[20];
    int frames = backtrace(callstack, 16);
    if (frames > 1)
    {
        logStack(&callstack[1], frames - 1, logGroup);
    }
#endif
}

class LoggableId
{
    const static int maxLength = 64;
    char _value[maxLength];
    static std::atomic<size_t> _lastInstanceId;
    const size_t _instanceId;

public:
    explicit LoggableId(const char* instancePrefix) : _instanceId(++_lastInstanceId)
    {
        snprintf(_value, maxLength, "%s-%zu", instancePrefix, _instanceId);
    }
    LoggableId(const char* prefix, size_t instanceId) : _instanceId(instanceId)
    {
        snprintf(_value, maxLength, "%s-%zu", prefix, instanceId);
    }

    ~LoggableId()
    {
        // debug("Deleted", c_str());
    }
    inline const char* c_str() const { return _value; }
    inline size_t getInstanceId() const { return _instanceId; }

    inline static size_t nextInstanceId() { return ++_lastInstanceId; }
};

} // namespace logger
