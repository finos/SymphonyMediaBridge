#pragma once
#include "concurrency/MpscQueue.h"
#include <atomic>
#include <chrono>
#include <string>
#include <sys/types.h>
#include <thread>

namespace logger
{

class LoggerThread
{
    struct LogItem
    {
        std::chrono::system_clock::time_point timestamp;
        const char* logLevel;
        void* threadId;
        char message[];
    };

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

    void stop();
    void reopen() { _reOpenLog.clear(); }

    void awaitLogDrained(float level, uint64_t timeoutNs);

    static void formatTime(const std::chrono::system_clock::time_point timestamp, char* output);

    uint32_t getDroppedLogCount() const { return _droppedLogs; }

private:
    void run();
    void reopenLogFile();
    void ensureLogFileExists();
    bool isTimeForMaintenance();
    bool isLogFileReopenNeeded();

    std::atomic_bool _running;
    std::atomic_flag _reOpenLog = ATOMIC_FLAG_INIT;
    concurrency::MpscQueue<LogItem> _logQueue;
    FILE* _logFile;
    ino_t _logFileINode;
    bool _logStdOut;
    std::string _logFileName;
    uint32_t _droppedLogs;
    uint64_t _lastMaintenanceTime;
    std::unique_ptr<std::thread> _thread; // must be last
};
} // namespace logger
