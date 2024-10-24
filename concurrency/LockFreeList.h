#pragma once
#include "VersionedPointer.h"
#include <atomic>

namespace concurrency
{
// Non copyable base class for items in the list
// Due to the nature of wait-free the ListItems put in the list
// must remain in memory also after being popped from the list.
// BEWARE! It is wise to make your sub class ListItem at least 64B total to avoid cache line contention
class ListItem
{
public:
    ListItem() { _next = VersionedPtr<ListItem>(); }

private:
    std::atomic<VersionedPtr<ListItem>> _next;
    friend class LockFreeList;
};

// Wait free, thread safe read and write.
class LockFreeList
{
    static_assert(sizeof(ListItem) % 8 == 0, "ListItem must align with 8 byte storage");

public:
    typedef ListItem NodeType;
    LockFreeList();
    ~LockFreeList() = default;

    bool push(ListItem* item);
    bool pop(ListItem*& item);

    bool empty() const { return _head.load().get() == &_eol; }
    uint32_t size() const { return _count; }

private:
    std::atomic<VersionedPtr<ListItem>> _head;
    uint64_t _cacheLineSeparator[7];
    std::atomic<VersionedPtr<ListItem>> _tail;
    ListItem _eol;
    std::atomic_uint32_t _count;
    static std::atomic_uint32_t _versionCounter;
};
} // namespace concurrency
