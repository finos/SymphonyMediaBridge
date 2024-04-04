#include "MpscQueue.h"
#include "memory/Allocator.h"

namespace concurrency
{

#ifdef DEBUG
MpscQueueBase::Guard::Guard(bool allocated)
{
    if (allocated)
    {
        std::memcpy(pattern, "ABXYGAHAERLKBOSP", 16);
    }
}
#endif

void MpscQueueBase::Entry::checkGuards() const
{
#ifdef DEBUG
    Guard a(true);

    assert(frontGuard == a);
    assert(tailGuard() == a);
#endif
}

void MpscQueueBase::Entry::clear()
{
#ifdef DEBUG
    frontGuard.clear();
    tailGuard().clear();
#endif
    // cppcheck-suppress memsetClass
    std::memset(this, State::emptySlot, entrySize()); // entire Entry must be cleared
}

MpscQueueBase::MpscQueueBase(uint32_t size) : _readCursor(0), _capacity(memory::page::alignedSpace(size))
{
    _queuestate = {0, 0};
    _data = reinterpret_cast<uint8_t*>(memory::page::allocate(_capacity));
    std::memset(_data, Entry::State::emptySlot, _capacity);
    assert(memory::isAligned<uint64_t>(reinterpret_cast<Entry*>(_data)->data));
}

MpscQueueBase::~MpscQueueBase()
{
    memory::page::free(_data, _capacity);
}

void* MpscQueueBase::front()
{
    Entry* entry = frontEntry();
    if (!entry)
    {
        return nullptr;
    }

    if (entry->state.load() == Entry::State::padding)
    {
        pop(entry);
        return front();
    }

    return &(entry->data);
}

uint32_t MpscQueueBase::frontSize() const
{
    auto entry = frontEntry();
    if (!entry)
    {
        return 0;
    }

    return entry->size;
}

void MpscQueueBase::pop()
{
    auto entry = frontEntry();
    if (!entry)
    {
        return;
    }

    pop(entry);
}

void* MpscQueueBase::allocate(uint32_t size)
{
    constexpr uint32_t mask = alignof(uint64_t) - 1;
    if (size & mask)
    {
        size = (size + alignof(uint64_t)) & ~mask;
    }

    const auto entrySize = size + Entry::entryOverHead();

    for (auto state = _queuestate.load(); entrySize + state.size <= _capacity;) //
    {
        if (isPaddingNeeded(state, entrySize))
        {
            state = pad(state);
            continue;
        }

        CursorState newState = state;
        newState.write = (newState.write + entrySize) % _capacity;
        newState.size += entrySize;
        assert(newState.size <= _capacity);
        if (_queuestate.compare_exchange_weak(state, newState))
        {
            auto& entry = reinterpret_cast<Entry&>(_data[state.write]);
            entry.size = size;
#ifdef DEBUG
            Guard zeroes;
            assert(entry.frontGuard == zeroes);
            assert(entry.tailGuard() == zeroes);
            Guard a(true);
            entry.frontGuard = a;
            entry.tailGuard() = a;
            assert(memory::isAligned<uint64_t>(entry.data));
#endif
            assert(entry.entrySize() == entrySize);
            entry.state.store(Entry::State::allocated);
            return entry.data;
        }
    }

    return nullptr;
}

void MpscQueueBase::commit(void* p)
{
    auto entry = Entry::fromPtr(p);
#ifdef DEBUG
    entry->checkGuards();
#endif
    entry->state.store(Entry::State::committed);
}

MpscQueueBase::Entry* MpscQueueBase::frontEntry()
{
    auto entry = reinterpret_cast<Entry*>(&_data[_readCursor]);
    if (entry->state.load() <= Entry::State::allocated)
    {
        return nullptr;
    }
    assert(_queuestate.load().size >= entry->entrySize());
    return entry;
}

MpscQueueBase::CursorState MpscQueueBase::pad(CursorState originalState)
{
    const uint32_t padding = _capacity - originalState.write;
    CursorState newState = originalState;
    newState.write = 0;
    newState.size += padding;
    assert(newState.size <= _capacity);

    for (CursorState state = originalState;;)
    {
        if (_queuestate.compare_exchange_weak(state, newState))
        {
            auto& entry = reinterpret_cast<Entry&>(_data[state.write]);
            entry.size = padding - Entry::entryOverHead();
#ifdef DEBUG
            Guard zeroes;
            assert(entry.frontGuard == zeroes);
            assert(entry.tailGuard() == zeroes);
            Guard a(true);
            entry.frontGuard = a;
            entry.tailGuard() = a;
#endif
            entry.state.store(Entry::State::padding);
            return newState;
        }
        if (originalState != state)
        {
            return state; // padding may not be necessary
        }
    }
}

bool MpscQueueBase::isPaddingNeeded(CursorState cursor, uint32_t entrySize) const
{
    const uint32_t spaceLeft = _capacity - cursor.write;
    if (spaceLeft >= entrySize + Entry::entryOverHead() || spaceLeft == entrySize)
    {
        return false;
    }
    return true;
}

void MpscQueueBase::pop(Entry* entry)
{
    assert(entry);

    const uint32_t entrySize = entry->entrySize();
#ifdef DEBUG
    entry->checkGuards();
#endif

    // entries are variable size so the cell state can end up anywhere and it must already be set emptySlot
    entry->clear();

    _readCursor = (_readCursor + entrySize) % _capacity;
    for (auto state = _queuestate.load();;)
    {
        CursorState newState = state;
        assert(state.size >= entrySize);
        newState.size -= entrySize;
        if (_queuestate.compare_exchange_weak(state, newState))
        {
            return;
        }
    }
}
} // namespace concurrency
