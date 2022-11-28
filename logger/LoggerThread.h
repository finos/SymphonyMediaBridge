#pragma once
#include "concurrency/MpmcQueue.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace logger
{
constexpr const int MAX_LINE_LENGTH = 4096;

struct LogItem
{
    std::chrono::system_clock::time_point timestamp;
    char message[MAX_LINE_LENGTH];
    const char* logLevel;
    void* threadId;
};

class LoggerThread
{
public:
    LoggerThread(FILE* logFile, bool logStdOut, size_t backlogSize);

    void post(const LogItem& item) { _logQueue.push(item); }
    void immediate(const LogItem& item);
    void flush();
    void stop();

    void awaitLogDrained(float level);

private:
    void run();
    void formatTime(const LogItem& item, char* output);

    std::atomic_bool _running;
    concurrency::MpmcQueue<LogItem> _logQueue;
    FILE* _logFile;
    bool _logStdOut;
    std::unique_ptr<std::thread> _thread;
};
} // namespace logger
