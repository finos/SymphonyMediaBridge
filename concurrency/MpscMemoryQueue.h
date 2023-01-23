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
 * Useful as wait free queue for variable size items in mpsc setting.
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

    struct Guard
    {
        Guard() {}
        explicit Guard(bool allocated)
        {
            if (allocated)
            {
                std::memcpy(pattern, "ABXYGAHAERLKBOSP", 16);
            }
        }

        bool operator==(const Guard& o) const { return 0 == std::memcmp(pattern, o.pattern, sizeof(pattern)); }

        char pattern[16] = {0};
    };

    struct Entry
    {
        Entry() : state(CellState::emptySlot) {}
        size_t entrySize() const { return entryOverHead() + size; }
        static constexpr uint32_t headSize() { return sizeof(Entry); }
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

        void checkGuards() const;
        Guard& tailGuard() { return *reinterpret_cast<Guard*>(data + size); }
        const Guard& tailGuard() const { return *reinterpret_cast<const Guard*>(data + size); }
#ifdef DEBUG
        Guard frontGuard;
#endif
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

    ~MpscMemoryQueue();

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
        auto state = _queuestate.load();
        return state.size;
    }

    bool empty() const { return size() == 0; }

    void clear() { _queuestate = {0, 0}; }

    uint32_t capacity() const { return _capacity; }

private: // methods
    Entry* frontEntry();
    const Entry* frontEntry() const { return const_cast<MpscMemoryQueue*>(this)->frontEntry(); }

    CursorState pad(CursorState originalState);
    bool isPaddingNeeded(CursorState cursor, uint32_t entrySize) const;
    void pop(Entry* entry);

private:
    std::atomic<CursorState> _queuestate;
    uint32_t _readCursor;
    uint8_t* _data;
    const size_t _capacity;
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
