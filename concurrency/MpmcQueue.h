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

            if (_readCursor.compare_exchange_weak(pos, pos + 1))
            {
                // we own the read position now. If thread pauses here,
                // the write cannot reach here until the state is set empty.
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
                // The emptyslot state will prevent read cursor and thereby write cursor to get here.
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

    void clear() {
        T elem;
        while(pop(elem));
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
