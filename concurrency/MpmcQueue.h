#pragma once
#include "memory/Allocator.h"
#include <algorithm>
#include <atomic>
#include <cassert>
namespace concurrency
{
// It is wise to make your queue entries at least 56B large
// to facilitate cache line separation
template <typename T>
class MpmcQueue
{
    enum CellState : int
    {
        emptySlot,
        committed
    };
    struct Entry
    {
        Entry() : state(emptySlot) {}

        std::atomic<CellState> state;
        T value;
    };

public:
    typedef T value_type;
    explicit MpmcQueue(uint32_t maxElements)
        : _maxElements(maxElements),
          _blockSize(memory::page::alignedSpace(maxElements * sizeof(Entry)))
    {
        _readCursor = 0;
        _writeCursor = 0;
        assert(0x100000000ull % _maxElements == 0); // "size % 2^32 must be zero";

        _cacheLineSeparator1[0] = 0;
        _cacheLineSeparator2[0] = 0;

        _elements = reinterpret_cast<Entry*>(memory::page::allocate(_blockSize));

        for (uint32_t i = 0; i < _maxElements; ++i)
        {
            new (&_elements[i]) Entry();
        }
    }

    ~MpmcQueue()
    {
        for (uint32_t i = 0; i < _maxElements; ++i)
        {
            _elements[i].~Entry();
        }

        memory::page::free(_elements, _blockSize);
    }

    // return false if empty
    bool pop(T& target)
    {
        uint32_t pos = _readCursor;
        for (;;)
        {
            if (!isReadable(pos))
            {
                if (pos == _readCursor)
                {
                    return false;
                }
                pos = _readCursor;
                continue;
            }

            if (_readCursor.compare_exchange_weak(pos, pos + 1))
            {
                // we own the read position now. If thread pauses here,
                // A writer cannot write due to committed state and will not increase write cursor.
                // A reader fail to update readCursor and has to try next slot.
                // A reader that wrapped will not pass readable test because the pos == writepos
                auto& entry = _elements[pos % _maxElements];
                target = std::move(entry.value);
                entry.state.store(CellState::emptySlot, std::memory_order_release);
                return true;
            }
        }
    }

    // return false if full
    // to reduce spin contention, avoid filling queue to full
    bool push(T&& obj)
    {
        uint32_t pos = _writeCursor;
        for (;;)
        {
            if (!isWritable(pos))
            {
                if (pos == _writeCursor)
                {
                    return false;
                }
                pos = _writeCursor;
                continue;
            }

            if (_writeCursor.compare_exchange_weak(pos, pos + 1))
            {
                // Assume we pause here.
                // The emptyslot state will stop read cursor.
                // The writeCursor has moved and another writer will have to try the next slot.
                // If writers wrap back here, they cannot write because q is full as read cursor could not pass this
                // point.
                auto& entry = _elements[pos % _maxElements];
                entry.value = std::move(obj);
                entry.state.store(CellState::committed, std::memory_order_release);
                return true;
            }
        }
    }

    template <typename... U>
    bool push(U&&... args)
    {
        return push(T(std::forward<U>(args)...));
    }

    size_t size() const
    {
        return std::max(0,
            static_cast<int32_t>(
                _writeCursor.load(std::memory_order_relaxed) - _readCursor.load(std::memory_order_relaxed)));
    }

    bool full() const { return !isWritable(_writeCursor.load(std::memory_order_consume)); }

    bool empty() const { return !isReadable(_readCursor.load(std::memory_order_consume)); }

    void clear()
    {
        if (!empty())
        {
            T elem;
            while (pop(elem))
                ;
        }
    }

    uint32_t capacity() const { return _maxElements; }

private:
    const uint32_t _maxElements;
    std::atomic_uint32_t _readCursor;
    uint64_t _cacheLineSeparator1[7];
    std::atomic_uint32_t _writeCursor;
    uint64_t _cacheLineSeparator2[7];
    Entry* _elements;
    const size_t _blockSize;

    bool isWritable(const uint32_t pos) const
    {
        return _elements[pos % _maxElements].state.load(std::memory_order_consume) == CellState::emptySlot &&
            pos - _readCursor.load(std::memory_order_consume) < _maxElements;
    }

    bool isReadable(const uint32_t pos) const
    {
        return _elements[pos % _maxElements].state.load(std::memory_order_consume) == CellState::committed &&
            pos != _writeCursor.load(std::memory_order_consume);
    }
};
} // namespace concurrency
