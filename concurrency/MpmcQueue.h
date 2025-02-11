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
            committed,
            nullSlot // this a especial slot for when queue is zero capacity, Which make the slot unreadable and
                     // unwritable
        } state = emptySlot;
    };

    struct EntrySate
    {
        EntrySate() {}

        EntrySate(typename CellState::State state) : state(CellState{0, state}) {}

        std::atomic<CellState> state;
    };

    struct Entry : EntrySate
    {
        Entry() {}

        T& value() { return reinterpret_cast<T&>(data); }
        alignas(alignof(T)) uint8_t data[sizeof(T)];
    };

    Entry* nullEntry()
    {
        // Make _elements to point to nullEntry (which is both unreadable and unwritable)
        // Also we need to const_cast. As _elements can't be const but we know for
        // empty queues, they will not change
        return reinterpret_cast<Entry*>(const_cast<EntrySate*>(&_nullEntry));
    }

    static constexpr size_t kEntryPerCacheLine = (63 + sizeof(Entry)) / sizeof(Entry);

    static constexpr uint32_t calculateFinalCapacity(uint32_t capacity)
    {
        return capacity == 0 ? 0 : (kEntryPerCacheLine * ((capacity + kEntryPerCacheLine - 1) / kEntryPerCacheLine));
    }

    static size_t calculateBlockSize(uint32_t finalCapacity)
    {
        return memory::page::alignedSpace(finalCapacity * sizeof(Entry));
    }

    struct VersionedIndex
    {
        uint32_t version = 0;
        uint32_t pos = 0;

        bool operator==(const VersionedIndex& i) const { return version == i.version && pos == i.pos; }
    };

    uint32_t indexTransform(VersionedIndex v) const
    {
        if constexpr (kEntryPerCacheLine == 1)
        {
            return v.pos % _capacity;
        }
        else
        {
            const auto p = v.pos * kEntryPerCacheLine;
            return (p / _capacity + p) % _capacity;
        }
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
        : _capacity(
              capacity == 0 ? 0 : (kEntryPerCacheLine * ((capacity + kEntryPerCacheLine - 1) / kEntryPerCacheLine))),
          _elements(capacity == 0 ? nullEntry()
                                  : reinterpret_cast<Entry*>(memory::page::allocate(calculateBlockSize(capacity))))
    {

        assert(_capacity == 0 || _capacity > 7);
        assert(capacity < 0x80000000u);
        assert(_capacity % kEntryPerCacheLine == 0);

        _readCursor = VersionedIndex();
        _writeCursor = VersionedIndex();

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

        if (_capacity)
        {
            memory::page::free(_elements, calculateBlockSize(_capacity));
        }
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
                // If writers wrap back here, element is not writable as the version is not the expected one
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
                // If writers wrap back here, element is not writable as the version is not the expected one
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
    alignas(64) std::atomic<VersionedIndex> _readCursor;

    // Special Cell state for when queue has zero size
    // Null Entry is not written and it is only read it when queue is zero size. So it can live in same cache line of
    // _readCursor without performance impact
    const EntrySate _nullEntry = CellState::State::nullSlot;

    alignas(64) std::atomic<VersionedIndex> _writeCursor;

    // Although they are const. Cache can be invalidated for the reader when _writeCursor
    // Perhaps is not a big issue (and even compile can optimize it this const values)
    // but forcing a new cache line also ensures tha nothing is placed in this cache line after
    // MpmcQueue boundary (which we can predict if can affect performance
    // of _writeCursor)
    alignas(64) const uint32_t _capacity;
    Entry* const _elements;
};
} // namespace concurrency
