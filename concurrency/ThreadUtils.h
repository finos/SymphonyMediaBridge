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
}