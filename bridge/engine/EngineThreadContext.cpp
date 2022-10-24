#include "EngineThreadContext.h"

using namespace bridge;

size_t EngineThreadContext::processEngineTasks(size_t maxTasksToProcess)
{
    size_t processedTasks = 0;
    const size_t tasksToProcess = std::min(_threadTasksQueue.size(), maxTasksToProcess);

    for (size_t i = 0; i < tasksToProcess; ++i)
    {
        utils::Function task;
        // We could specialize thread task queue so we can execute
        // without moving memory from container to outside
        if (!_threadTasksQueue.pop(task))
        {
            break;
        }

        task();
        ++processedTasks;
    }

    return processedTasks;
}