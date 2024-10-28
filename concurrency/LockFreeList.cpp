#include "LockFreeList.h"
#include <atomic>
namespace concurrency
{

std::atomic_uint32_t LockFreeList::_versionCounter(1);
LockFreeList::LockFreeList() : _count(0)
{
    _head.store(VersionedPtr<ListItem>(&_eol, 0));
    _tail.store(VersionedPtr<ListItem>(&_eol, 0));
    _eol._next = VersionedPtr<ListItem>(&_eol, 0);
    _cacheLineSeparator[0] = 0xBA;
}

// Possible to push a list of items too
bool LockFreeList::push(ListItem* item)
{
    const uint32_t version = _versionCounter.fetch_add(1);
    int count = 0;

    auto itemNode = VersionedPtr<ListItem>(item, version);
    VersionedPtr<ListItem> lastNode;
    for (auto p = itemNode; p; p = p->_next.load())
    {
        ++count;
        auto nextPtr = p->_next.load(std::memory_order::memory_order_acquire);
        if (!nextPtr)
        {
            lastNode = VersionedPtr<ListItem>(p.get(), version);
            p->_next.store(VersionedPtr<ListItem>(&_eol, version), std::memory_order::memory_order_release);
            break;
        }
        else
        {
            p->_next.store(VersionedPtr<ListItem>(nextPtr.get(), version), std::memory_order::memory_order_release);
        }
    }

    // move tail to our new tail, retry until we make it
    auto prevTailNode = _tail.load();
    auto prevTailNext = prevTailNode->_next.load();
    while (!_tail.compare_exchange_weak(prevTailNode, lastNode))
    {
        prevTailNext = prevTailNode->_next;
    }

    // prevTail may be one of: &this->_eol, prev node that may be in list or popped already
    // prevTailNext may be one of: &this->_eol, nullptr, a new node in any list, &_eol in any list
    // if tail next is not eol of this list, it was popped and we attach to head instead
    // if it was popped and reinserted and next is still _eol, the version will differ, and we attach to head.
    // Otherwise we would detach ourselves and the rest. if we set next first, popper must notice and move head

    if (prevTailNode.get() != &_eol && prevTailNext.get() == &_eol &&
        prevTailNode->_next.compare_exchange_strong(prevTailNext, itemNode))
    {
        // if we won this, the popper will have to move head to us
        _count.fetch_add(count, std::memory_order::memory_order_relaxed);
        return true;
    }
    else
    {
        // prevTailNext is now indicating either nullptr, eol in another list, eol in this list but another version so
        // re-inserted at end popped empty, we must attach to head
        _head.store(itemNode, std::memory_order_release);
        _count.fetch_add(count, std::memory_order::memory_order_relaxed);
        return true;
    }
}

/* scenarios:
 empty list, head points to _eol
    return false

Otherwise, we load _head, next of head, and try to update _head to point to next. That will fail if _head changed and we
retry.
- Once popped, we cannot tell if we popped the _tail also. We do not know if others are pushing to our next.
- We try to update _tail in case it pointed to our node, but it may have moved on and there is nothing we can do.
It may have moved on before we checked it.
- Now if we fail to update our next from the next pointer we had before pop, it means nodes have been added to our next
because our node was the tail node at some point. But that also means we moved the _head to eol. All we can do is to
have _head point to the new list added to our tail.

*/
bool LockFreeList::pop(ListItem*& item)
{
    VersionedPtr<ListItem> nextNode;
    auto nodeToPop = _head.load(std::memory_order::memory_order_acquire);
    for (;;)
    {
        if (nodeToPop.get() == &_eol)
        {
            return false;
        }

        nextNode = nodeToPop->_next.load();
        if (!nextNode) // cheap check if node is popped, instead of CAS
        {
            nodeToPop = _head.load();
            continue;
        }

        if (_head.compare_exchange_weak(nodeToPop, nextNode))
        {
            break;
        }
    }

    {
        auto tail = _tail.load();
        if (tail == nodeToPop)
        {
            _tail.compare_exchange_strong(tail, nextNode);
            // else tail moved ahead already
        }
    }

    // Neither head nor tail can point to this node now. We own it.
    // But a pusher may have added to our tail, before we changed _tail
    if (!nodeToPop->_next.compare_exchange_strong(nextNode, VersionedPtr<ListItem>()))
    {
        // A tail was added to our node. We have to fix _head since we moved it to eol
        _head.store(nextNode);
        nodeToPop->_next.store(VersionedPtr<ListItem>(), std::memory_order::memory_order_release);
    }

    item = nodeToPop.get();
    _count.fetch_sub(1, std::memory_order::memory_order_relaxed);
    return true;
}

} // namespace concurrency
