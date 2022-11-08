#pragma once

#include "concurrency/MpmcQueue.h"
#include "utils/Function.h"

namespace concurrency
{

class SynchronizationContext
{
public:
    SynchronizationContext(concurrency::MpmcQueue<utils::Function>& queue) : _queue(queue) {}

    bool post(utils::Function&& task) { return _queue.push(std::move(task)); }
    bool post(const utils::Function& task) { return _queue.push(task); }

private:
    concurrency::MpmcQueue<utils::Function>& _queue;
};

} // namespace concurrency
