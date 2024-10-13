#include "WaitFreeStack.h"
#include <atomic>
#include <cassert>
namespace concurrency
{

WaitFreeStack::WaitFreeStack()
{
    _cacheLinePadding[0] = 0xBA; // silence compile warning
}

void WaitFreeStack::push(StackItem* item)
{
    assert(item != nullptr);
    if (item == nullptr)
    {
        return;
    }

    const auto versionedNext = item->_next.load(std::memory_order_relaxed);
    assert(versionedNext.get() == nullptr);
    auto itemNode = VersionedPtr<StackItem>(item, versionedNext.version() + 1);
    for (auto currentNode = _head.load(std::memory_order_consume);;)
    {
        item->_next.store(currentNode, std::memory_order_relaxed);
        if (_head.compare_exchange_weak(currentNode, itemNode))
        {
            return;
        }
    }
}

bool WaitFreeStack::pop(StackItem*& item)
{
    for (auto currentNode = _head.load(std::memory_order_consume);;)
    {
        auto pCurrent = currentNode.get();
        if (pCurrent == nullptr)
        {
            return false;
        }

        auto nextNode = pCurrent->_next.load(std::memory_order_relaxed);
        if (_head.compare_exchange_weak(currentNode, nextNode))
        {
            // next is used to store version counter for this node
            pCurrent->_next.store(VersionedPtr<StackItem>(nullptr, currentNode.version()), std::memory_order_relaxed);
            item = pCurrent;
            return true;
        }
    }
}

} // namespace concurrency
