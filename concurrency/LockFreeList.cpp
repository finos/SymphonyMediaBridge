#include "LockFreeList.h"
#include <atomic>
namespace concurrency
{

std::atomic_uint32_t LockFreeList::_versionCounter(1);
LockFreeList::LockFreeList() : _head(&_eol), _tail(&_eol), _count(0)
{
    _eol._next = &_eol;
    _cacheLineSeparator[0] = 0xBA;
}

// Possible to push a list of items too
bool LockFreeList::push(ListItem* item)
{
    ListItem* last = nullptr;
    const uint32_t version = _versionCounter.fetch_add(1);
    int count = 0;

    for (auto p = item; p; p = p->_next.load())
    {
        ++count;
        const auto nextPtr = p->_next.load(std::memory_order::memory_order_relaxed);
        if (nextPtr == nullptr)
        {
            last = p;
            p->_next.store(makeVersionedPointer(&_eol, version));
            break;
        }
        else
        {
            p->_next.store(makeVersionedPointer(nextPtr, version));
        }
    }

    auto itemNode = makeVersionedPointer(item, version);
    auto lastNode = makeVersionedPointer(last, version);

    // move tail to our new tail, retry until we make it
    auto prevTailNode = _tail.load();
    auto pPrevTail = getPointer(prevTailNode);
    auto prevTailNext = pPrevTail->_next.load();
    while (!_tail.compare_exchange_weak(prevTailNode, lastNode))
    {
        pPrevTail = getPointer(prevTailNode);
        prevTailNext = pPrevTail->_next;
    }

    // prevTail may be one of: &this->_eol, prev node that may be in list or popped already
    // prevTailNext may be one of: &this->_eol, nullptr, a new node in any list, &_eol in any list
    // if tail next is not eol of this list, it was popped and we attach to head instead
    // if it was popped and reinserted and next is still _eol, the version will differ, and we attach to head.
    // Otherwise we would detach ourselves and the rest. if we set next first, popper must notice and move head

    if (pPrevTail != &_eol && getPointer(prevTailNext) == &_eol &&
        pPrevTail->_next.compare_exchange_strong(prevTailNext, itemNode))
    {
        // if we won this, the popper will have to move head to us
        _count.fetch_add(count, std::memory_order::memory_order_relaxed);
        return true;
    }
    else
    {
        // prevTailNext is now indicating either nullptr, eol in another list, eol in this list but another version so
        // re-inserted at end popped empty, we must attach to head
        _head.store(itemNode);
        _count.fetch_add(count, std::memory_order::memory_order_relaxed);
        return true;
    }
}

/* scenarios:
 empty list, head points to _eol
    return false

    Otherwise, we iterate down the list and try to lok an item. If a next pointer is not pointing to node of proper
 version we restart from head. Version always have to be checked after locking.

 = Popping the tail item =
    if we locked tail item we can try to set tail back to head but it may already have been set to a new tail
    We can try to set the head past our item but it may have been moved to earlier item or later item. In case of
    earlier, we retry. If head has been moved back to eol we cannot decide unless head ptr is still versioned. eol
 item does not have to be versioned.

 = Popping first item or mid item
    There is no difference. Once item is locked we try to move the pointer to next as long as the version of pointer
 is less than our item's version

*/
bool LockFreeList::pop(ListItem*& item)
{
    ListItem* pCurrent = nullptr;
    ListItem* nextNode = nullptr;
    ListItem* currentNode = _head.load(std::memory_order::memory_order_consume);
    for (;;)
    {
        pCurrent = getPointer(currentNode);
        if (pCurrent == &_eol)
        {
            return false;
        }

        nextNode = pCurrent->_next.load();
        if (nextNode == nullptr) // cheap check instead of CAS
        {
            currentNode = _head.load();
            continue;
        }

        if (_head.compare_exchange_weak(currentNode, nextNode))
        {
            break;
        }
    }
    // we won the node for popping and the next pointer is from that time

    {
        auto tail = _tail.load();
        if (tail == currentNode)
        {
            _tail.compare_exchange_strong(tail, nextNode);
            // else tail moved ahead already
        }
    }

    // neither head nor tail can point to this node. We own it.
    // but a pusher may be trying to add to next
    if (!pCurrent->_next.compare_exchange_strong(nextNode, nullptr))
    {
        // pusher won. tail was added to our node. We have to fix head since we moved it to eol
        _head.store(nextNode);
        pCurrent->_next.store(nullptr, std::memory_order::memory_order_relaxed);
    }

    item = pCurrent;
    _count.fetch_sub(1, std::memory_order::memory_order_relaxed);
    return true;
}

} // namespace concurrency
