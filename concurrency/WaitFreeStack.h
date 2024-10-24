#pragma once
#include "VersionedPointer.h"
#include <atomic>

namespace concurrency
{
// Non copyable base class for items in the stack
// Due to the nature of wait-free the StackItems put in the list
// must remain in memory also after being popped from the list.
// BEWARE! Make the sub class of StackItem at least 64B total to separate cache lines
class StackItem
{
public:
    StackItem() { _next = VersionedPtr<StackItem>(); }
    StackItem(const StackItem&) = delete;

private:
    std::atomic<VersionedPtr<StackItem>> _next;
    friend class WaitFreeStack;
};

// Wait free, thread safe read and write.
class WaitFreeStack
{
    static_assert(sizeof(StackItem) % 8 == 0, "ListItem must align with 8 byte storage");

public:
    typedef StackItem NodeType;
    WaitFreeStack();
    ~WaitFreeStack() = default;

    void push(StackItem* item);
    bool pop(StackItem*& item);

    bool empty() const { return !_head.load(); }

private:
    std::atomic<VersionedPtr<StackItem>> _head;
    uint64_t _cacheLinePadding[7];
};
} // namespace concurrency
