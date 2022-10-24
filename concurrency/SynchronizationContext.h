#pragma once

#include "concurrency/MpmcQueue.h"
#include "utils/Function.h"

namespace concurrency
{

class SynchronizationContext
{
public:
    SynchronizationContext(concurrency::MpmcQueue<utils::Function>& queue) : _queue(queue) {}

    void post(utils::Function&& task) { _queue.push(std::move(task)); }
    void post(const utils::Function& task) { _queue.push(task); }

private:
    concurrency::MpmcQueue<utils::Function>& _queue;
};

} // namespace concurrency