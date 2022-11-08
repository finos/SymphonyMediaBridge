#pragma once
#include <atomic>

namespace concurrency
{

// publishes a value
// multiple consumers and multiple publishers
// wait-free. writers are never stalled, readers will read somewhat older information at worst.
template <typename T, int MAX_THREADS = 32>
class MpmcPublish
{
    struct Entry
    {
        Entry() : refCount(0) {}

        std::atomic_uint32_t refCount;
        T value;
    };

    static constexpr uint32_t indexBits = 16;
    static constexpr uint32_t TOO_MANY_THREADS = 1 << (indexBits - 1);
    static constexpr uint32_t indexMask = (1 << indexBits) - 1;

    uint32_t indexOf(uint32_t cursor) const { return cursor & indexMask; }
    uint32_t versionOf(uint32_t cursor) const { return cursor >> indexBits; }
    uint32_t toCursor(uint32_t version, uint32_t index) const { return (version << indexBits) | 0x80000000 | index; }

public:
    typedef T ValueType;
    MpmcPublish() : _readCursor(0)
    {
        static_assert(MAX_THREADS > 0 && MAX_THREADS < TOO_MANY_THREADS,
            "entries in MpmcPublish cannot exceed TOO_MANY_THREADS");
    }

    ~MpmcPublish() {}

    // return false if empty
    bool read(T& target) const
    {
        if (empty())
        {
            return false;
        }

        for (;;)
        {
            const uint32_t readIndex = indexOf(_readCursor.load(std::memory_order::memory_order_consume));
            Entry& cell = const_cast<Entry&>(_elements[readIndex]);
            if (cell.refCount.fetch_add(1) < TOO_MANY_THREADS)
            {
                target = cell.value;
                cell.refCount.fetch_sub(1);
                return true;
            }
            else
            {
                cell.refCount.fetch_sub(1);
            }
        }
    }

    // iterate until we find an idle slot to write into.
    // most reading threads will be locking the previous write pos
    bool write(const T& obj)
    {
        uint32_t readCursor = _readCursor.load(std::memory_order::memory_order_relaxed);
        const uint32_t readIndex = indexOf(readCursor);
        const uint32_t version = versionOf(readCursor);
        for (int i = 0; i < MAX_THREADS - 1; ++i)
        {
            const auto index = (i + readIndex) % MAX_THREADS;
            Entry& cell = _elements[index];
            uint32_t expectedCount = 0;
            if (cell.refCount.compare_exchange_strong(expectedCount, TOO_MANY_THREADS))
            {
                cell.value = obj;
                cell.refCount.fetch_sub(TOO_MANY_THREADS - 1); // make it readable before pointing readpointer to it
                _readCursor.compare_exchange_strong(readCursor, toCursor(version + 1, index));
                cell.refCount.fetch_sub(1);
                return true;
            }
        }

        return false;
    }

    bool empty() const { return _readCursor.load(std::memory_order_consume) == 0; }

private:
    std::atomic_uint32_t _readCursor;
    Entry _elements[MAX_THREADS];
};
} // namespace concurrency
