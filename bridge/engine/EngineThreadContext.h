#pragma once

#include "concurrency/MpmcQueue.h"
#include "utils/Function.h"

namespace bridge
{

class EngineThreadContext
{
protected:
    EngineThreadContext(size_t taskQueueSize) : _threadTasksQueue(taskQueueSize) {}

public:
    virtual ~EngineThreadContext() = default;

    void post(utils::Function&& task) { _threadTasksQueue.push(std::move(task)); }
    void post(const utils::Function& task) { _threadTasksQueue.push(task); }

protected:
    /** Process task queue and returns the number of tasks processed */
    size_t processEngineTasks(size_t maxTasksToProcess);

private:
    concurrency::MpmcQueue<utils::Function> _threadTasksQueue;
};

} // namespace bridge