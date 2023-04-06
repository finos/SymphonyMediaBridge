#pragma once
#include <atomic>
#include <cassert>
#include <iterator>

namespace concurrency
{

// publishes a value
// multiple consumers and multiple publishers
// wait-free. writers are never stalled, readers will read somewhat older information at worst.
template <typename T, size_t MAX_THREADS = 32>
class MpmcPublish
{
    static const size_t SLOTS = MAX_THREADS + 1;
    struct Entry
    {
        Entry() : refCount(0) {}

        std::atomic_uint32_t refCount; // 0 means free
        T value;
    };

public:
    typedef T ValueType;
    MpmcPublish() : _readPointer(nullptr)
    {
        static_assert(MAX_THREADS > 2 && MAX_THREADS < 512, "entries must > 2 < 512 in MpmcPublish");
    }

    ~MpmcPublish() {}

    // return false if empty
    bool read(T& target) const
    {
        for (auto* cell = _readPointer.load(std::memory_order_consume); cell;
             cell = _readPointer.load(std::memory_order_consume))
        {
            const auto prevCount = cell->refCount.fetch_add(1);
            if (prevCount > 0)
            {
                target = cell->value;
                cell->refCount.fetch_sub(1);
                return true;
            }

            cell->refCount.fetch_sub(1);
        }

        return false;
    }

    // iterate until we find an idle slot to write into.
    // most reading threads will be locking the previous write pos
    bool write(const T& obj)
    {
        auto readPointer = _readPointer.load(std::memory_order_consume);
        const uint32_t offset = readPointer ? std::distance(&_elements[0], readPointer) + 1 : 0;
        for (size_t i = 0; i < SLOTS * 20; ++i)
        {
            Entry& writeCell = _elements[(i + offset) % SLOTS];
            if (writeCell.refCount.fetch_add(1) == 0)
            {
                // it is mine now
                writeCell.value = obj;
                if (!_readPointer.compare_exchange_strong(readPointer, &writeCell, std::memory_order_release))
                {
                    writeCell.refCount.fetch_sub(1);
                    // ok, we lost race
                }
                else if (readPointer)
                {
                    readPointer->refCount.fetch_sub(1);
                }
                return true;
            }
            else
            {
                writeCell.refCount.fetch_sub(1);
            }
        }

        return false; // failed to allocate write slot
    }

    bool empty() const { return _readPointer.load(std::memory_order_consume) == nullptr; }

private:
    std::atomic<Entry*> _readPointer;
    // all threads + one published slot.
    Entry _elements[SLOTS];
};
} // namespace concurrency
