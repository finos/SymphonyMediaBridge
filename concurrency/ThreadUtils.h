#pragma once
#include <thread>
namespace concurrency
{
enum class Priority
{
    Normal,
    RealTime
};
bool setPriority(std::thread& thread, Priority priority);
void setThreadName(const char* name);

void getThreadName(char* name, size_t& length);
void getThreadName(pthread_t threadId, char* name, size_t& length);
} // namespace concurrency
