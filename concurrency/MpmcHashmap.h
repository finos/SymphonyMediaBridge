#pragma once
#include "LockFreeList.h"
#include "memory/Allocator.h"
#include "memory/details.h"
#include "utils/StdExtensions.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <vector>

namespace concurrency
{

// wait-free
// Keys may be 0
// No value may be zero !!
// key is 40bits and will be truncated to 40 bits.
// elementCount must be power of two and <= 16777216
// Since key is prehashed and not stored in the index, there is a risk of hash collision and you will only notice by
// add returning false. You cannot then add the item.
class MurmurHashIndex
{
public:
    explicit MurmurHashIndex(size_t elementCount);

    bool add(uint64_t key, uint32_t value);
    bool remove(uint64_t key);
    bool get(uint64_t key, uint32_t& value) const;
    bool containsKey(uint64_t key) const;

    uint32_t removeNext(uint32_t index, uint32_t& position);
    size_t capacity() const { return _index.size(); }

    // not thread safe
    void reInitialize();

private:
    uint32_t position(uint64_t hashValue, uint32_t offset) const { return (hashValue + offset) % _index.size(); }

    // Key and value will be stored atomically and is therefore exactly 64 bits.
    // A value of zero means empty slot

    static const uint64_t KEY_MASK = (uint64_t(1) << 40) - 1;
    struct KeyValue
    {
        KeyValue() : key(0), value(0) {}
        KeyValue(uint64_t key_, uint32_t value_) : key(key_ & KEY_MASK), value(value_) {}

        uint64_t key : 40;
        uint64_t value : 24;
    };

    class Entry
    {
    public:
        Entry() : keyValue(KeyValue()), cacheLineSeparator(0) { cacheLineSeparator = 0; }
        std::atomic<KeyValue> keyValue;

    private:
        uint64_t cacheLineSeparator;
        // There is expected spread in vector so cache line contention should be small already.
    };

    void updateSpread(uint32_t i);

    std::vector<Entry> _index;
    std::atomic_uint32_t _maxSpread;
};

// how much larger do we make the hash index to reduce collisions

// wait-free
// bounded
// mpmc
// optimistic reuse once all empty slots are consumed.
// If recycling is too intensive you will step into corrupt data.
// Be careful when recycling near container capacity.
// key is 40bit max
// Calling the destructor of the element stored is delayed until the
// slot is recycled or hashmap is destroyed. This is to reduce risk of
// bad access when items are removed.
// If key type or value type requires heap alloc construction, it is no longer wait free
template <typename KeyT, typename T>
class MpmcHashmap32
{
    enum class State : uint32_t
    {
        empty, // under construction
        committed,
        tombstone
    };

    struct Entry : public ListItem
    {
        template <typename... Args>
        explicit Entry(const KeyT& key, Args&&... args)
            : element(std::piecewise_construct,
                  std::forward_as_tuple(key),
                  std::forward_as_tuple(std::forward<Args>(args)...))
        {
            assert(state.is_lock_free());
        }

        std::atomic<State> state;
        std::pair<KeyT, T> element;
    };

public:
    template <typename ValueType>
    class IterBase
    {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = ValueType;
        using difference_type = std::size_t;
        using pointer = ValueType*;
        using reference = ValueType&;

        IterBase(Entry* entries, uint32_t pos, uint32_t endPos) : _elements(entries), _pos(pos), _end(endPos) {}
        IterBase(const IterBase& it) : _elements(it._elements), _pos(it._pos), _end(it._end) {}

        IterBase& operator++()
        {
            if (_pos == _end)
            {
                return *this; // cannot advance a logical end iterator
            }

            ++_pos;
            while (_pos != _end && _elements[_pos].state.load() != State::committed)
            {
                ++_pos;
            }

            return *this;
        }

        IterBase& operator--()
        {
            if (_pos == 0)
            {
                return *this; // cannot retreat a logical begin iterator
            }

            --_pos;
            while (_pos != _end && _elements[_pos].state.load() != State::committed)
            {
                --_pos;
            }

            return *this;
        }

        ValueType& operator*() { return _elements[_pos].element; }
        ValueType* operator->() { return &_elements[_pos].element; }
        const ValueType& operator*() const { return _elements[_pos].element; }
        const ValueType* operator->() const { return &_elements[_pos].element; }
        bool operator==(const IterBase& it) const { return _pos == it._pos || (isEnd() && it.isEnd()); }

        bool operator!=(const IterBase& it) const
        {
            if (!isEnd() && !it.isEnd())
            {
                return _pos != it._pos;
            }
            return isEnd() != it.isEnd();
        }

    private:
        bool isEnd() const { return _pos == _end; }
        Entry* _elements;
        uint32_t _pos;
        uint32_t _end;
    };

    typedef const IterBase<std::pair<KeyT, T>> const_iterator;
    typedef IterBase<std::pair<KeyT, T>> iterator;
    typedef std::pair<KeyT, T> value_type;
    using PointerType = typename std::conditional<std::is_pointer<T>::value, T, std::remove_reference_t<T>*>::type;

    explicit MpmcHashmap32(size_t maxElements) : _end(0), _capacity(maxElements), _index(maxElements * 4)
    {
        if (_capacity == 0)
        {
            _elements = nullptr;
            return;
        }

        void* mem = memory::page::allocate(memory::page::alignedSpace(_capacity * sizeof(Entry)));

        _elements = reinterpret_cast<Entry*>(mem);
        assert(memory::isAligned<std::max_align_t>(_elements));

        for (size_t i = 0; i < _capacity; ++i)
        {
            _elements[i].state = State::empty;
            _freeItems.push(&_elements[i]);
        }
    }

    ~MpmcHashmap32()
    {
        for (size_t i = 0; i < _end.load(); ++i)
        {
            if (_elements[i].state.load() != State::empty)
            {
                _elements[i].~Entry();
            }
        }

        if (_capacity)
        {
            memory::page::free(_elements, memory::page::alignedSpace(_capacity * sizeof(Entry)));
        }
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(const KeyT& key, Args&&... args)
    {
        {
            auto existingIt = find(key);
            if (existingIt != end())
            {
                return std::make_pair(existingIt, false);
            }
        }

        Entry* reusedEntry = nullptr;
        ListItem* listItem = nullptr;
        if (_freeItems.pop(listItem))
        {
            reusedEntry = reinterpret_cast<Entry*>(listItem);
            auto state = reusedEntry->state.load();
            if (state == State::empty)
            {
                _end.fetch_add(1);
            }
            else if (state == State::tombstone)
            {
                reusedEntry->~Entry();
            }
        }
        else
        {
            return std::make_pair(end(), false);
        }

        const uint32_t pos = std::distance(_elements, reinterpret_cast<Entry*>(reusedEntry));
        auto entry = new (reusedEntry) Entry(key, std::forward<Args>(args)...);
        entry->state.store(State::committed);
        if (!_index.add(utils::hash<KeyT>{}(key), pos + 1))
        {
            // index must be full or duplicate key
            entry->state.store(State::tombstone);
            _freeItems.push(entry);

            auto existingIt = find(key);
            return std::make_pair(existingIt, false);
        }

        return std::make_pair(iterator(_elements, pos, pos + 1), true);
    }

    bool erase(const KeyT& key)
    {
        const uint64_t key64 = utils::hash<KeyT>{}(key);
        uint32_t pos = 0;
        if (!_index.get(key64, pos))
        {
            return false;
        }
        assert(pos > 0);

        if (_index.remove(key64))
        {
            --pos;
            _elements[pos].state.store(State::tombstone);
            _freeItems.push(&_elements[pos]);
            return true;
        }

        return false;
    }

    bool contains(const KeyT& key) const { return _index.containsKey(utils::hash<KeyT>{}(key)); }

    iterator find(const KeyT& key)
    {
        uint32_t pos = 0;
        if (_index.get(utils::hash<KeyT>{}(key), pos))
        {
            --pos;
            if (_elements[pos].state.load() == State::committed)
            {
                return iterator(_elements, pos, pos + 1);
            }
            else
            {
                return end();
            }
        }
        return end();
    }

    const_iterator find(const KeyT& key) const
    {
        auto& map = const_cast<MpmcHashmap32<KeyT, T>&>(*this);
        auto it = map.find(key);
        return const_iterator(it);
    }

    // concurrent version
    void clear()
    {
        for (size_t i = 0; i < _index.capacity();)
        {
            uint32_t pos;
            i = _index.removeNext(i, pos);
            if (pos != 0)
            {
                --pos;
                _elements[pos].state.store(State::tombstone);
                _freeItems.push(&_elements[pos]);
            }
        }
    }

    // not thread safe
    void reInitialize()
    {
        _index.reInitialize();
        ListItem* item = nullptr;
        while (_freeItems.pop(item))
        {
        }

        for (size_t i = 0; i < _capacity; ++i)
        {
            if (_elements[i].state.load() != State::empty)
            {
                _elements[i].~Entry();
            }

            _elements[i].state.store(State::empty);
            _freeItems.push(&_elements[i]);
        }
        _end = 0;
    }

    size_t capacity() const { return _capacity; }
    size_t size() const { return _capacity - _freeItems.size(); }
    bool empty() const { return _capacity == _freeItems.size(); }

    const_iterator cbegin() const
    {
        uint32_t first = 0;
        const uint32_t theEnd = _end.load(std::memory_order_acquire);
        while (first != theEnd && _elements[first].state.load() != State::committed)
        {
            ++first;
        }
        return const_iterator(_elements, first, theEnd);
    }

    const_iterator cend() const
    {
        const uint32_t currentEnd = _end.load(std::memory_order_acquire);
        return const_iterator(_elements, currentEnd, currentEnd);
    }

    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator begin()
    {
        uint32_t first = 0;
        const uint32_t theEnd = _end.load(std::memory_order_acquire);
        while (first != theEnd && _elements[first].state.load() != State::committed)
        {
            ++first;
        }
        return iterator(_elements, first, theEnd);
    }
    iterator end()
    {
        const uint32_t currentEnd = _end.load(std::memory_order_acquire);
        return iterator(_elements, currentEnd, currentEnd);
    }

    PointerType getItem(const KeyT& key)
    {
        auto it = find(key);
        if (it != end())
        {
            return memory::detail::pointerOf(it->second);
        }
        return nullptr;
    }

    const PointerType getItem(const KeyT& key) const { return const_cast<MpmcHashmap32<KeyT, T>&>(*this).getItem(key); }

private:
    Entry* _elements;
    std::atomic_uint32_t _end;

    const uint32_t _capacity;

    MurmurHashIndex _index;

    LockFreeList _freeItems;
};
} // namespace concurrency
