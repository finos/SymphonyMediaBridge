#include "WaitFreeStack.h"
#include <atomic>
#include <cassert>
namespace concurrency
{

WaitFreeStack::WaitFreeStack() : _head(nullptr)
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
    assert(getPointer(versionedNext) == nullptr);
    auto itemNode = makeVersionedPointer(item, getVersion(versionedNext) + 1);
    for (StackItem* currentNode = _head.load(std::memory_order_consume);;)
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
    for (StackItem* currentNode = _head.load(std::memory_order_consume);;)
    {
        StackItem* pCurrent = getPointer(currentNode);
        if (pCurrent == nullptr)
        {
            return false;
        }

        StackItem* nextNode = pCurrent->_next.load(std::memory_order_relaxed);
        if (_head.compare_exchange_weak(currentNode, nextNode))
        {
            // next is used to store version counter for this node
            pCurrent->_next.store(makeVersionedPointer<StackItem>(nullptr, getVersion(currentNode)),
                std::memory_order_relaxed);
            item = pCurrent;
            return true;
        }
    }
}

} // namespace concurrency
