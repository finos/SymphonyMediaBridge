#pragma once
#include "memory/Allocator.h"
#include <algorithm>
#include <atomic>
#include <cassert>
namespace concurrency
{

// High performing thread safe Multiple Producer, Multiple Consumer, Wait-free queue but...
// If a reader is pre-empted when reading the element and a writer catches up, the writer will fail to push as the write
// slot is still being read. This happens typically when the queue is rather full and there are competing threads in the
// system.
template <typename T>
class MpmcQueue
{
    struct CellState
    {
        uint32_t version = 0;
        enum State : uint32_t
        {
            emptySlot = 0,
            committed
        } state = emptySlot;
    };

    struct Entry
    {
        Entry() {}

        T& value() { return reinterpret_cast<T&>(data); }
        std::atomic<CellState> state;
        uint8_t data[sizeof(T)];
    };

    struct VersionedIndex
    {
        uint32_t version = 0;
        uint32_t pos = 0;

        bool operator==(const VersionedIndex& i) const { return version == i.version && pos == i.pos; }
    };

    uint32_t indexTransform(VersionedIndex v) const
    {
        if (_entriesPerCacheline == 1)
        {
            return v.pos % _capacity;
        }

        const auto p = v.pos * _entriesPerCacheline;
        return (p / _capacity + p) % _capacity;
    }

    VersionedIndex nextPosition(VersionedIndex index) const
    {
        if (index.pos + 1 < _capacity)
        {
            ++index.pos;
            return index;
        }
        else
        {
            ++index.version;
            index.pos = 0;
            return index;
        }
    }

public:
    typedef T value_type;
    explicit MpmcQueue(uint32_t capacity)
        : _entriesPerCacheline((63 + sizeof(Entry)) / sizeof(Entry)),
          _capacity(_entriesPerCacheline * ((capacity + _entriesPerCacheline - 1) / _entriesPerCacheline)),
          _blockSize(memory::page::alignedSpace(_capacity * sizeof(Entry))),
          _elements(reinterpret_cast<Entry*>(memory::page::allocate(_blockSize)))
    {
        assert(_capacity > 7);
        assert(capacity < 0x80000000u);
        assert(_capacity % _entriesPerCacheline == 0);

        _readCursor = VersionedIndex();
        _writeCursor = VersionedIndex();

        _cacheLineSeparator1[0] = 0;
        _cacheLineSeparator2[0] = 0;

        for (uint32_t i = 0; i < _capacity; ++i)
        {
            new (&_elements[i]) Entry();
        }
    }

    ~MpmcQueue()
    {
        for (uint32_t i = 0; i < _capacity; ++i)
        {
            if (_elements[i].state.load().state == CellState::State::committed)
            {
                _elements[i].value().~T();
            }
            _elements[i].~Entry();
        }

        memory::page::free(_elements, _blockSize);
    }

    bool pop(T& target)
    {
        for (auto pos = _readCursor.load(std::memory_order_consume);;)
        {
            if (!isReadable(pos))
            {
                const auto readCursor = _readCursor.load(std::memory_order_consume);
                if (pos == readCursor)
                {
                    return false;
                }
                pos = readCursor;
                continue;
            }

            const auto newPos = nextPosition(pos);

            if (_readCursor.compare_exchange_weak(pos, newPos, std::memory_order_seq_cst))
            {
                // we own the read position now. If thread pauses here,
                // A writer cannot write due to committed state and will not increase write cursor.
                // A reader fail to update readCursor and has to try next slot.
                // A reader that wrapped will not pass readable test because the readcursor version does not match cell
                // state version
                auto& entry = _elements[indexTransform(pos)];
                target = std::move(entry.value());
                entry.value().~T();
                entry.state.store(CellState{pos.version + 1, CellState::emptySlot}, std::memory_order_release);
                return true;
            }
        }
    }

    bool push(T&& obj)
    {
        for (auto pos = _writeCursor.load(std::memory_order_consume);;)
        {
            if (!isWritable(pos))
            {
                const auto writeCursor = _writeCursor.load(std::memory_order_consume);
                if (pos == writeCursor)
                {
                    return false;
                }
                pos = writeCursor;
                continue;
            }

            const auto newPos = nextPosition(pos);

            if (_writeCursor.compare_exchange_weak(pos, newPos, std::memory_order_seq_cst))
            {
                // Assume we pause here.
                // The emptyslot state will stop read cursor.
                // The writeCursor has moved and another writer will have to try the next slot.
                // If writers wrap back here, element is not writeable as the version is not the expected one
                auto& entry = _elements[indexTransform(pos)];
                new (entry.data) T(std::move(obj));
                entry.state.store(CellState{pos.version, CellState::committed}, std::memory_order_release);
                return true;
            }
        }
    }

    template <typename... U>
    bool push(U&&... args)
    {
        for (auto pos = _writeCursor.load(std::memory_order_consume);;)
        {
            if (!isWritable(pos))
            {
                const auto writeCursor = _writeCursor.load(std::memory_order_consume);
                if (pos == writeCursor)
                {
                    return false;
                }
                pos = writeCursor;
                continue;
            }

            const auto newPos = nextPosition(pos);

            if (_writeCursor.compare_exchange_weak(pos, newPos, std::memory_order_seq_cst))
            {
                // Assume we pause here.
                // The emptyslot state will stop read cursor.
                // The writeCursor has moved and another writer will have to try the next slot.
                // If writers wrap back here, element is not writeable as the version is not the expected one
                auto& entry = _elements[indexTransform(pos)];
                new (entry.data) T(std::forward<U>(args)...);
                entry.state.store(CellState{pos.version, CellState::committed}, std::memory_order_release);
                return true;
            }
        }
    }

    // will return correct size if queue is not in motion.
    size_t size() const
    {
        const auto writePos = _writeCursor.load(std::memory_order_relaxed);
        const auto readPos = _readCursor.load(std::memory_order_relaxed);

        if (writePos == readPos)
        {
            return 0; // empty atm
        }
        else if (writePos.pos == readPos.pos)
        {
            return _capacity;
        }

        return std::max(uint32_t(1), (_capacity + writePos.pos - readPos.pos) % _capacity);
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

    uint32_t capacity() const { return _capacity; }

private:
    bool isWritable(const VersionedIndex& index) const
    {
        const auto cell = _elements[indexTransform(index)].state.load(std::memory_order_consume);
        return cell.state == CellState::emptySlot && cell.version == index.version;
    }

    bool isReadable(const VersionedIndex& index) const
    {
        const auto cell = _elements[indexTransform(index)].state.load(std::memory_order_consume);
        return cell.state == CellState::committed && cell.version == index.version;
    }

    // Layout below must be maintained for performance. The non atomic member variables may not be place before read
    // cursor.
    std::atomic<VersionedIndex> _readCursor;
    uint32_t _cacheLineSeparator1[14];
    std::atomic<VersionedIndex> _writeCursor;
    uint32_t _cacheLineSeparator2[14];
    const uint32_t _entriesPerCacheline;
    const uint32_t _capacity;
    const size_t _blockSize;
    Entry* const _elements;
};
} // namespace concurrency
