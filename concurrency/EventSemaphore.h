#pragma once
#include <mutex>

namespace concurrency
{

/*
    Releases all threads waiting and keeps EventSemaphore open until reset.
*/
class EventSemaphore
{
public:
    EventSemaphore() {}
    EventSemaphore(const EventSemaphore&) = delete;

    void post();
    bool await(int timeoutMs);
    void await();
    void reset();

private:
    std::mutex _mutex;
    std::condition_variable _condition;
    int _flag = 0;
};

} // namespace concurrency