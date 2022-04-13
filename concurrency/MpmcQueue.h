#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <sys/mman.h>
#include <unistd.h>
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
        Entry() : state(emptySlot), padding(0) {}

        std::atomic<CellState> state;
        uint32_t padding;
        T value;
    };

public:
    typedef T value_type;
    explicit MpmcQueue(uint32_t maxElements) : _maxElements(maxElements), _blockSize(calculateNeededSpace(maxElements))
    {
        _readCursor = 0;
        _writeCursor = 0;
        assert(0x100000000ull % _maxElements == 0); // "size % 2^32 must be zero";

        _cacheLineSeparator1[0] = 0;
        _cacheLineSeparator2[0] = 0;

        _elements = reinterpret_cast<Entry*>(
            mmap(nullptr, _blockSize, (PROT_READ | PROT_WRITE), (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0));
        assert(reinterpret_cast<intptr_t>(_elements) != -1);

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
        munmap(_elements, _blockSize);
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

            if (pos == _writeCursor.load(std::memory_order_consume))
            {
                return false; // we wrapped and caught up with a pending read
            }

            if (_readCursor.compare_exchange_weak(pos, pos + 1))
            {
                // we own the read position now. If thread pauses here,
                // a writer cannot write due to commited state.
                // A reader fail to update readCursor and has to try next slot.
                // If we wrap, writers cannot write into this slot but another reader will be able to read it!!
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

            if (pos - _readCursor.load(std::memory_order_consume) >= _maxElements)
            {
                return false; // it could be full and we wrapped
            }

            if (_writeCursor.compare_exchange_weak(pos, pos + 1))
            {
                // Assume we pause here.
                // The emptyslot state will stop read cursor.
                // The _writeCursor has moved and another writer will have to try the next slot.
                // Since read cursor is stopped on reading this, we cannot wrap write cursor
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

    bool full() const
    {
        auto pos = _writeCursor.load(std::memory_order_consume);
        return !isWritable(pos) || pos - _readCursor.load(std::memory_order_consume) >= _maxElements;
    }

    bool empty() const
    {
        auto pos = _readCursor.load(std::memory_order_consume);
        return !isReadable(pos) || pos == _writeCursor.load(std::memory_order_consume);
    }

    void clear()
    {
        if (!empty())
        {
            T elem;
            while (pop(elem))
                ;
        }
    }

private:
    const uint32_t _maxElements;
    std::atomic_uint32_t _readCursor;
    uint64_t _cacheLineSeparator1[7];
    std::atomic_uint32_t _writeCursor;
    uint64_t _cacheLineSeparator2[7];
    Entry* _elements;
    const size_t _blockSize;

    static size_t calculateNeededSpace(uint32_t maxElements)
    {
        auto neededSize = sizeof(Entry) * maxElements;
        const auto pageSize = getpagesize();
        const auto remaining = neededSize % pageSize;
        return neededSize + (remaining != 0 ? pageSize - remaining : 0);
    }

    bool isWritable(const uint32_t pos) const
    {
        return _elements[pos % _maxElements].state.load(std::memory_order_acquire) == CellState::emptySlot;
    }

    bool isReadable(const uint32_t pos) const
    {
        return _elements[pos % _maxElements].state.load(std::memory_order_acquire) == CellState::committed;
    }
};
} // namespace concurrency
