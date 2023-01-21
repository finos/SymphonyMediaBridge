#pragma once
#include "utils/Allocator.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>

namespace concurrency
{
/**
 * Multiple producer single consumer queue of arbitrary large memory blocks.
 * This can be used to reduce memory consumption for queues with variable sized items.
 */
class MpscMemoryQueue
{
    enum CellState : uint32_t
    {
        emptySlot = 0,
        allocated,
        committed,
        padding
    };
    struct Entry
    {
        Entry() : state(CellState::emptySlot) {}
        size_t entrySize() const { return headSize() + size; }
        static constexpr uint32_t headSize() { return sizeof(uint32_t) * 2; }
        static Entry* fromPtr(void* p)
        {
            return reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(p) - sizeof(int32_t) * 2);
        }

        std::atomic<CellState> state;
        uint32_t size;
        uint8_t data[];
    };

    struct CursorState
    {
        uint32_t write = 0;
        uint32_t size;

        bool operator!=(const CursorState& o) const { return size != o.size || write != o.write; }
    };

public:
    explicit MpscMemoryQueue(uint32_t size);

    ~MpscMemoryQueue() { memory::page::free(_data, _blockSize); }

    void* front();
    template <class T>
    T* front()
    {
        return reinterpret_cast<T*>(front());
    }

    uint32_t frontSize() const;
    void pop();

    void* allocate(uint32_t size);
    void commit(void* p);

    size_t size() const
    {
        auto state = _cursor.load();
        return state.size;
    }

    bool empty() const { return size() == 0; }

    void clear() { _cursor = {0, 0}; }

    uint32_t capacity() const { return _blockSize; }

private: // methods
    Entry* frontEntry();
    const Entry* frontEntry() const { return const_cast<MpscMemoryQueue*>(this)->frontEntry(); }

    CursorState pad(CursorState originalState);
    bool isPaddingNeeded(CursorState cursor, uint32_t wantedAllocation) const;
    void pop(Entry* entry);

private:
    std::atomic<CursorState> _cursor;
    uint32_t _readCursor;
    uint8_t* _data;
    const size_t _blockSize;
};

class ScopedAllocCommit
{
public:
    ScopedAllocCommit(MpscMemoryQueue& heap, uint32_t size) : _heap(heap), _committed(false)
    {
        _ptr = heap.allocate(size);
    }
    ~ScopedAllocCommit() { commit(); }

    explicit operator bool() noexcept { return _ptr != nullptr; }
    void* operator->() { return _ptr; }
    template <class T>
    T& get()
    {
        return *reinterpret_cast<T*>(_ptr);
    }

    void commit()
    {
        if (_ptr && !_committed)
        {
            _heap.commit(_ptr);
            _committed = true;
        }
    }

private:
    MpscMemoryQueue& _heap;
    bool _committed;
    void* _ptr;
};
} // namespace concurrency
