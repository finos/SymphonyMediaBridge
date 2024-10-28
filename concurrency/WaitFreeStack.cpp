#include "WaitFreeStack.h"
#include <atomic>
#include <cassert>
namespace concurrency
{

WaitFreeStack::WaitFreeStack()
{
    _head = VersionedPtr<StackItem>();
    _cacheLinePadding[0] = 0xBA; // silence compile warning
}

void WaitFreeStack::push(StackItem* item)
{
    assert(item != nullptr);
    if (item == nullptr)
    {
        return;
    }

    const auto versionedNext = item->_next.load(std::memory_order_acquire);
    assert(!versionedNext);
    auto newNode = VersionedPtr<StackItem>(item, versionedNext.version() + 1);
    for (auto head = _head.load(std::memory_order_acquire);;)
    {
        item->_next.store(head, std::memory_order_release);
        if (_head.compare_exchange_weak(head, newNode))
        {
            return;
        }
    }
}

bool WaitFreeStack::pop(StackItem*& item)
{
    for (auto head = _head.load(std::memory_order_acquire);;)
    {
        if (!head)
        {
            return false;
        }

        auto nextNode = head->_next.load(std::memory_order_acquire);
        if (_head.compare_exchange_weak(head, nextNode))
        {
            // next is used to store version counter for this node
            head->_next.store(VersionedPtr<StackItem>(nullptr, head.version()), std::memory_order_relaxed);
            item = head.get();
            return true;
        }
    }
}

} // namespace concurrency
