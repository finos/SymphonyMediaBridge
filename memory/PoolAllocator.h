#pragma once

#include "concurrency/LockFreeList.h"
#include "concurrency/WaitFreeStack.h"
#include "logger/Logger.h"
#include <cassert>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

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
    static size_t calculateNeededSpace(size_t desiredCount)
    {
        auto finalSize = desiredCount * sizeof(Entry);
        const auto pageSize = getpagesize();
        const auto remaining = finalSize % pageSize;
        return finalSize + (remaining != 0 ? pageSize - remaining : 0);
    }

public:
    PoolAllocator(size_t elementCount, const std::string&& name)
        : _name(std::move(name)),
          _size(calculateNeededSpace(elementCount)),
          _originalElementCount(_size / sizeof(Entry))
    {
        _count = _originalElementCount;
        _popIndex = 0;
        _pushIndex = 0;
        _cacheLineSeparator1[0] = 0;
        _cacheLineSeparator2[0] = 0;
        _cacheLineSeparator3[0] = 0;

        _elements = reinterpret_cast<Entry*>(
            mmap(nullptr, _size, (PROT_READ | PROT_WRITE), (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0));
        assert(reinterpret_cast<intptr_t>(_elements) != -1);

        static_assert(sizeof(Entry) % 8 == 0, "ELEMENT_SIZE must be multiple of alignment");

        for (size_t i = 0; i < _originalElementCount; ++i)
        {
            new (&_elements[i]) Entry();
            _freeQueue[i % QCOUNT].push(&(_elements[i]));
        }
    }

    ~PoolAllocator()
    {
#if DEBUG
        if (_originalElementCount != size())
        {
            logger::warn("leaked pool allocator elements %zu", _name.c_str(), _originalElementCount - size());
            for (size_t i = 0; i < _originalElementCount; ++i)
            {
                const Entry& entry = _elements[i];
                if (entry._beginGuard == 0xEFEFEFEFEFEFEFEFLLU)
                {
                    logger::warn("element leak at %p", _name.c_str(), &entry);
                    logger::logStack(entry._callStack + 1, 9, _name.c_str());
                }
            }
        }
#endif
        munmap(_elements, _size);
    }

    PoolAllocator(const PoolAllocator&) = default;
    PoolAllocator& operator=(const PoolAllocator&) = default;
    PoolAllocator(PoolAllocator&&) = default;
    PoolAllocator& operator=(PoolAllocator&&) = default;
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
#ifdef DEBUG
        _count.fetch_sub(1, std::memory_order_relaxed);
#endif
        auto entry = reinterpret_cast<Entry*>(item);
#ifdef DEBUG
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
        auto entry = reinterpret_cast<Entry*>(reinterpret_cast<char*>(pointer) - sizeof(typename Entry::Head));
#ifdef DEBUG
        assert(reinterpret_cast<uintptr_t>(entry) >= reinterpret_cast<uintptr_t>(_elements) &&
            reinterpret_cast<uintptr_t>(entry) < reinterpret_cast<uintptr_t>(_elements) + _size);

        assert(entry->_beginGuard == 0xEFEFEFEFEFEFEFEFLLU);
        assert(entry->_endGuard == 0xFEFEFEFEFEFEFEFELLU);
        entry->_beginGuard = 0xABABABABABABABABLLU;
        entry->_endGuard = 0xBABABABABABABABALLU;
#endif
        const auto index = _pushIndex.fetch_add(1) % QCOUNT;
        _freeQueue[index].push(entry);
#ifdef DEBUG
        _count.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    static bool isCorrupt(void* pointer)
    {
#ifdef DEBUG
        auto entry = reinterpret_cast<Entry*>(reinterpret_cast<char*>(pointer) - sizeof(typename Entry::Head));

        return (entry->_beginGuard != 0xEFEFEFEFEFEFEFEFLLU || entry->_endGuard != 0xFEFEFEFEFEFEFEFELLU);
#else
        return false;
#endif
    }

private:
#ifdef DEBUG
    class Head : public concurrency::StackItem
    {
    public:
        Head() { _callStack[0] = nullptr; }
        void* _callStack[10];
        uint64_t _beginGuard = 0xABABABABABABABABLLU;
    };
#else
    class Head : public concurrency::StackItem
    {
    };
#endif
    class Entry : public Head
    {
    public:
        Entry() { _data[0] = 0; }

        uint8_t _data[ELEMENT_SIZE];
#ifdef DEBUG
        uint64_t _endGuard = 0xBABABABABABABABALLU;
#endif
    };

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
