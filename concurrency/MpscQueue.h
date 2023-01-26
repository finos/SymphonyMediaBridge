#pragma once
#include "memory/Allocator.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>

namespace concurrency
{

class MpscQueueBase
{
    struct Guard
    {
        Guard() {}
        explicit Guard(bool allocated);
        void clear() { std::memset(pattern, 0, sizeof(pattern)); }
        bool operator==(const Guard& o) const { return 0 == std::memcmp(pattern, o.pattern, sizeof(pattern)); }

        char pattern[16] = {0};
    };
    static_assert(sizeof(Guard) % alignof(std::max_align_t) == 0,
        "MpscQueueBase::Guard must allow proper alignment of data");

    struct Entry
    {
        enum State : uint32_t
        {
            emptySlot = 0,
            allocated,
            committed,
            padding
        };

        Entry() : state(State::emptySlot), size(0) {}
        size_t entrySize() const { return entryOverHead() + size; }
        static constexpr uint32_t entryOverHead()
        {
#ifdef DEBUG
            return sizeof(Entry) + sizeof(Guard);
#else
            return sizeof(Entry);
#endif
        }

        static Entry* fromPtr(void* p)
        {
            return reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(p) - sizeof(Entry));
        }
        void clear();
        void checkGuards() const;
        Guard& tailGuard() { return *reinterpret_cast<Guard*>(data + size); }
        const Guard& tailGuard() const { return *reinterpret_cast<const Guard*>(data + size); }
#ifdef DEBUG
        Guard frontGuard;
#endif
        std::atomic<State> state;
        uint32_t size;
        uint8_t data[];
    };
    static_assert(sizeof(Entry) % alignof(uint64_t) == 0, "Entry must allow 64bit alignment of data");

    struct CursorState
    {
        uint32_t write = 0;
        uint32_t size;

        bool operator!=(const CursorState& o) const { return size != o.size || write != o.write; }
    };

public:
    explicit MpscQueueBase(uint32_t size);

    ~MpscQueueBase();

    void* front();
    uint32_t frontSize() const;
    void pop();

    void* allocate(uint32_t size);
    void commit(void* p);

    size_t size() const
    {
        auto state = _queuestate.load();
        return state.size;
    }

    bool empty() const { return size() == 0; }

    void clear() { _queuestate = {0, 0}; }

    uint32_t capacity() const { return _capacity; }

private: // methods
    Entry* frontEntry();
    const Entry* frontEntry() const { return const_cast<MpscQueueBase*>(this)->frontEntry(); }

    CursorState pad(CursorState originalState);
    bool isPaddingNeeded(CursorState cursor, uint32_t entrySize) const;
    void pop(Entry* entry);

private:
    std::atomic<CursorState> _queuestate;
    uint32_t _readCursor;
    uint8_t* _data;
    const size_t _capacity;
};

/**
 * Multiple producer single consumer queue of arbitrary large memory blocks.
 * Useful as wait free queue for variable size items in mpsc setting.
 * You can allocate variable size objects in the queue by using the size
 * parameter in allocate.
 */
template <typename T>
class MpscQueue
{
public:
    MpscQueue(uint32_t sizeInBytes) : _queue(sizeInBytes) {}

    T* allocate(uint32_t size = sizeof(T))
    {
        auto p = _queue.allocate(size);
        if (p)
        {
            return new (p) T();
        }
        return nullptr;
    }

    void commit(T* p) { _queue.commit(p); }

    T* front() { return reinterpret_cast<T*>(_queue.front()); }
    uint32_t frontSize() const { return _queue.frontSize(); }
    void pop() { _queue.pop(); }

    size_t size() const { return _queue.size(); }
    bool empty() const { return size() == 0; }
    uint32_t capacity() const { return _queue.capacity(); }

    void clear() { _queue.clear(); }

private:
    MpscQueueBase _queue;
};

/**
 * This is similar to a smart pointer but the purpose is to assure allocate and commit phase of an item in
 * MpscQueue. An allocated item must be commited or the queue popping will halt.
 */
template <typename T>
class ScopedAllocCommit
{
public:
    explicit ScopedAllocCommit(MpscQueue<T>& heap, uint32_t size = sizeof(T)) : _heap(heap), _committed(false)
    {
        _ptr = heap.allocate(size);
    }
    ~ScopedAllocCommit() { commit(); }

    explicit operator bool() noexcept { return _ptr != nullptr; }
    T* operator->() { return _ptr; }
    T& operator*() { return *_ptr; }

    void commit()
    {
        if (_ptr && !_committed)
        {
            _heap.commit(_ptr);
            _committed = true;
        }
    }

private:
    MpscQueue<T>& _heap;
    bool _committed;
    T* _ptr;
};
} // namespace concurrency
