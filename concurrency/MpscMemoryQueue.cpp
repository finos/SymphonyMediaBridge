#include "MpscMemoryQueue.h"
#include "utils/Allocator.h"

namespace concurrency
{
MpscMemoryQueue::MpscMemoryQueue(uint32_t size) : _readCursor(0), _blockSize(memory::page::alignedSpace(size))
{
    assert(0x100000000ull % _blockSize == 0);
    _cursor = {0, 0};

    _data = reinterpret_cast<uint8_t*>(memory::page::allocate(_blockSize));
    std::memset(_data, emptySlot, _blockSize);
}

void* MpscMemoryQueue::front()
{
    Entry* entry = frontEntry();
    if (!entry)
    {
        return nullptr;
    }

    if (entry->state.load() == CellState::padding)
    {
        pop(entry);
        return front();
    }

    return &(entry->data);
}

uint32_t MpscMemoryQueue::frontSize() const
{
    auto entry = frontEntry();
    if (!entry)
    {
        return 0;
    }

    return entry->size;
}

void MpscMemoryQueue::pop()
{
    auto entry = frontEntry();
    if (!entry)
    {
        return;
    }

    pop(entry);
}

void* MpscMemoryQueue::allocate(uint32_t size)
{
    constexpr uint32_t mask = sizeof(uint64_t) - 1;
    if (size & mask)
    {
        size = (size + sizeof(uint64_t)) & ~mask;
    }

    const auto entrySize = size + Entry::headSize();
    // must check if we need to instert padding
    // must check if queue is full
    for (auto state = _cursor.load(); entrySize + state.size <= _blockSize;)
    {
        if (isPaddingNeeded(state, size))
        {
            state = pad(state);
            continue;
        }

        CursorState newState = state;
        newState.write += entrySize;
        newState.size += entrySize;
        assert(newState.size <= _blockSize);
        if (_cursor.compare_exchange_weak(state, newState))
        {
            auto& entry = reinterpret_cast<Entry&>(_data[state.write % _blockSize]);
            entry.size = size;
            entry.state.store(CellState::allocated);
            return entry.data;
        }
    }

    return nullptr;
}

void MpscMemoryQueue::commit(void* p)
{
    auto entry = Entry::fromPtr(p);
    entry->state.store(CellState::committed);
}

MpscMemoryQueue::Entry* MpscMemoryQueue::frontEntry()
{
    auto entry = reinterpret_cast<Entry*>(&_data[_readCursor % _blockSize]);
    if (entry->state.load() <= allocated)
    {
        return nullptr;
    }
    assert(_cursor.load().size >= entry->entrySize());
    return entry;
}

MpscMemoryQueue::CursorState MpscMemoryQueue::pad(CursorState originalState)
{
    const uint32_t remainder = originalState.write % _blockSize;
    CursorState newState = originalState;
    const uint32_t padding = _blockSize - remainder;
    newState.write += padding;
    newState.size += padding;
    assert(newState.size <= _blockSize);

    for (CursorState state = originalState;;)
    {
        if (_cursor.compare_exchange_weak(state, newState))
        {
            auto& entry = reinterpret_cast<Entry&>(_data[state.write % _blockSize]);
            entry.size = padding - Entry::headSize();
            entry.state.store(CellState::padding);
            return newState;
        }
        if (originalState != state)
        {
            return state; // padding may not be necessary
        }
    }
}

bool MpscMemoryQueue::isPaddingNeeded(CursorState cursor, uint32_t wantedAllocation) const
{
    const uint32_t pos = cursor.write % _blockSize;
    const uint32_t spaceLeft = _blockSize - pos;

    if (spaceLeft >= wantedAllocation + 2 * Entry::headSize() || spaceLeft == wantedAllocation + Entry::headSize())
    {
        return false;
    }
    return true;
}

void MpscMemoryQueue::pop(Entry* entry)
{
    assert(entry);

    const uint32_t entrySize = entry->entrySize();

    // entries are variable size so the cell state can end up anywhere and it must already be set emptySlot
    std::memset(entry, CellState::emptySlot, entrySize);

    _readCursor += entrySize;
    for (auto state = _cursor.load();;)
    {
        CursorState newState = state;
        assert(state.size >= entrySize);
        newState.size -= entrySize;
        if (_cursor.compare_exchange_weak(state, newState))
        {
            return;
        }
    }
}
} // namespace concurrency
