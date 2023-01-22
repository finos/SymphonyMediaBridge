#pragma once
#include "concurrency/MpscMemoryQueue.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace logger
{
constexpr const int MAX_LINE_LENGTH = 4096;

struct LogItem
{
    std::chrono::system_clock::time_point timestamp;
    const char* logLevel;
    void* threadId;
    char message[1];
};

class LoggerThread
{
public:
    LoggerThread(const char* logFileName, bool logStdOut, size_t backlogSize);

    void post(std::chrono::system_clock::time_point timestamp,
        const char* logLevel,
        const char* logGroup,
        void* threadId,
        const char* format,
        va_list args);

    void immediate(std::chrono::system_clock::time_point timestamp,
        const char* logLevel,
        const char* logGroup,
        void* threadId,
        const char* format,
        va_list args);

    void flush();
    void stop();
    void reopen() { _reOpenLog.clear(); }

    void awaitLogDrained(float level);

    static void formatTime(const std::chrono::system_clock::time_point timestamp, char* output);

private:
    void run();
    void reopenLogFile();
    void ensureLogFileExists();

    std::atomic_bool _running;
    std::atomic_flag _reOpenLog = ATOMIC_FLAG_INIT;
    concurrency::MpscMemoryQueue _logQueue;
    FILE* _logFile;
    bool _logStdOut;
    std::string _logFileName;
    std::unique_ptr<std::thread> _thread; // must be last
};
} // namespace logger
