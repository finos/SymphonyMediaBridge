#pragma once

#include "EngineFunction.h"
#include "concurrency/MpmcQueue.h"

namespace bridge
{

class EngineThreadContext
{
protected:
    EngineThreadContext(size_t taskQueueSize) : _threadTasksQueue(taskQueueSize) {}

public:
    virtual ~EngineThreadContext() = default;

    void post(EngineFunction&& engineFunction) { _threadTasksQueue.push(std::move(engineFunction)); }
    void post(const EngineFunction& engineFunction) { _threadTasksQueue.push(engineFunction); }

protected:
    /** Process task queue and returns the number of tasks processed */
    size_t processEngineTasks(size_t maxTasksToProcess);

private:
    concurrency::MpmcQueue<EngineFunction> _threadTasksQueue;
};

} // namespace bridge