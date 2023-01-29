#pragma once

#include "concurrency/WaitFreeStack.h"
#include "logger/Logger.h"
#include "memory/Allocator.h"
#include <cassert>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <string>

#if !defined(ENABLE_ALLOCATOR_METRICS)
#ifdef DEBUG
#define ENABLE_ALLOCATOR_METRICS 1
#define POOLALLOC_MEMGUARDS 1
#else
#define ENABLE_ALLOCATOR_METRICS 0
#define POOLALLOC_MEMGUARDS 0
#endif
#endif

namespace memory
{

/**
    @brief
        Manages a pool of S elements of type T. PoolAllocator is thread safe.
*/
template <size_t ELEMENT_SIZE>
class PoolAllocator
{
    static const size_t QCOUNT = 8;

public:
    class Deleter
    {
    public:
        Deleter() : _allocator(nullptr) {}
        explicit Deleter(PoolAllocator<ELEMENT_SIZE>* allocator) : _allocator(allocator) {}

        template <typename T>
        void operator()(T* r)
        {
            assert(_allocator);
            if (_allocator)
            {
                _allocator->free(r);
            }
        }

        PoolAllocator<ELEMENT_SIZE>* _allocator;
    };

    PoolAllocator(size_t elementCount, const std::string&& name)
        : _deleter(this),
          _name(std::move(name)),
          _elements(nullptr),
          _popIndex(0),
          _pushIndex(0),
          _size(memory::page::alignedSpace(elementCount * sizeof(Entry))),
          _originalElementCount(_size / sizeof(Entry)),
          _count(_originalElementCount)
    {
        _cacheLineSeparator1[0] = 0;
        _cacheLineSeparator2[0] = 0;
        _cacheLineSeparator3[0] = 0;

        _elements = reinterpret_cast<Entry*>(memory::page::allocate(_size));
        assert(memory::isAligned<uint64_t>(_elements));

        static_assert(sizeof(Entry) % alignof(std::max_align_t) == 0, "ELEMENT_SIZE must be multiple of alignment");

        for (size_t i = 0; i < _originalElementCount; ++i)
        {
            auto entry = new (&_elements[i]) Entry();
            _freeQueue[i % QCOUNT].push(entry);
        }
    }

    ~PoolAllocator()
    {
        logAllocatedElements();
        memory::page::free(_elements, _size);
    }

    Deleter& getDeleter() { return _deleter; }

    void logAllocatedElements()
    {
#if POOLALLOC_MEMGUARDS
        if (_originalElementCount != size())
        {
            logger::warn("leaked pool allocator elements %zu", _name.c_str(), _originalElementCount - size());
            for (size_t i = 0; i < _originalElementCount; ++i)
            {
                const Entry& entry = _elements[i];
                if (entry._beginGuard == 0xEFEFEFEFEFEFEFEFLLU)
                {
                    logger::warnImmediate("element leak at %p", _name.c_str(), &entry);
                    logger::logStack(entry._callStack + 1, 9, _name.c_str());
                }
            }
        }
#endif
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;
    const std::string& getName() const { return _name; }

    size_t size() const { return _count.load(std::memory_order_relaxed); }
    size_t countAllocatedItems() const { return _originalElementCount - size(); }
    void* allocate()
    {
        concurrency::StackItem* item = nullptr;
        const auto index = _popIndex.fetch_add(1);

        if (!_freeQueue[index % QCOUNT].pop(item))
        {
#if DEBUG
            logger::errorImmediate("pool depleted", _name.c_str());
            logger::logStack(_name.c_str());
#endif
            return nullptr;
        }
#if ENABLE_ALLOCATOR_METRICS
        _count.fetch_sub(1, std::memory_order_relaxed);
#endif
        auto entry = reinterpret_cast<Entry*>(item);
#if POOLALLOC_MEMGUARDS
        assert(entry->_beginGuard == 0xABABABABABABABABLLU);
        assert(entry->_endGuard == 0xBABABABABABABABALLU);
        entry->_beginGuard = 0xEFEFEFEFEFEFEFEFLLU;
        entry->_endGuard = 0xFEFEFEFEFEFEFEFELLU;
        backtrace(entry->_callStack, 10);
#endif
        return entry->_data;
    }

    void free(void* pointer)
    {
        if (!pointer)
        {
            return;
        }

        auto entry = reinterpret_cast<Entry*>(reinterpret_cast<char*>(pointer) - Entry::headerSize());
#if POOLALLOC_MEMGUARDS
        assert(reinterpret_cast<uintptr_t>(entry) >= reinterpret_cast<uintptr_t>(_elements) &&
            reinterpret_cast<uintptr_t>(entry) < reinterpret_cast<uintptr_t>(_elements) + _size);

        assert(entry->_beginGuard == 0xEFEFEFEFEFEFEFEFLLU);
        assert(entry->_endGuard == 0xFEFEFEFEFEFEFEFELLU);
        entry->_beginGuard = 0xABABABABABABABABLLU;
        entry->_endGuard = 0xBABABABABABABABALLU;
#endif
        const auto index = _pushIndex.fetch_add(1) % QCOUNT;
        _freeQueue[index].push(entry);
#if ENABLE_ALLOCATOR_METRICS
        _count.fetch_add(1, std::memory_order_relaxed);
#endif
    }

protected:
    static bool isCorrupt(void* pointer)
    {
#if POOLALLOC_MEMGUARDS
        auto entry = reinterpret_cast<Entry*>(reinterpret_cast<char*>(pointer) - Entry::headerSize());

        return (entry->_beginGuard != 0xEFEFEFEFEFEFEFEFLLU || entry->_endGuard != 0xFEFEFEFEFEFEFEFELLU);
#else
        return false;
#endif
    }

private:
    class Head : public concurrency::StackItem
    {
#if POOLALLOC_MEMGUARDS
    public:
        Head() { _callStack[0] = nullptr; }
        void* _callStack[10];
        uint64_t _beginGuard = 0xABABABABABABABABLLU;
#endif
    };

    class Entry : public Head
    {
    public:
        Entry() { _data[0] = 0; }
        static size_t headerSize()
        {
            static const Entry e;
            static const size_t headerSize = e._data - reinterpret_cast<const uint8_t*>(&e);
            return headerSize;
        }

        alignas(std::max_align_t) uint8_t _data[ELEMENT_SIZE % alignof(std::max_align_t)
                ? ELEMENT_SIZE + alignof(std::max_align_t) - (ELEMENT_SIZE % alignof(std::max_align_t))
                : ELEMENT_SIZE];
#if POOLALLOC_MEMGUARDS
        uint64_t _endGuard = 0xBABABABABABABABALLU;
#endif
    };

    Deleter _deleter;
    std::string _name;
    Entry* _elements;
    uint64_t _cacheLineSeparator1[6];
    concurrency::WaitFreeStack _freeQueue[QCOUNT];
    std::atomic_uint32_t _popIndex;
    uint64_t _cacheLineSeparator2[7];
    std::atomic_uint32_t _pushIndex;
    uint64_t _cacheLineSeparator3[5];
    const size_t _size;
    const size_t _originalElementCount;
    std::atomic_uint32_t _count;
};

} // namespace memory
