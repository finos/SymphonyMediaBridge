#pragma once
#include "utils/StdExtensions.h"
#include <array>
#include <cinttypes>
#include <cstring>

namespace memory
{
namespace detail
{

template<class T>
std::enable_if_t<std::is_pointer<std::decay_t<T>>::value, T>
pointerOf(T&& value)
{
    return value;
}

template<class T>
std::enable_if_t<!std::is_pointer<std::decay_t<T>>::value, std::remove_reference_t<T>*>
pointerOf(T&& value)
{
    return &value;
}

} // namespace detail

/**
 * Heap free Map, completely stored on stack.
 * Note that key is hashed to 64 bit and it does not handle hash collisions.
 * If two keys hash to the same int64, only the first can be stored in the map.
 */
template <typename KeyT, typename T, uint32_t SIZE>
class Map
{
    using PointerType = std::conditional_t<std::is_pointer<T>::value, T, T*>;

    struct ElementEntry
    {
        std::pair<KeyT, T> keyValue;
        bool committed = false;
    };

    struct IndexEntry
    {
        IndexEntry() : keyHash(0), position(0) {}
        IndexEntry(uint64_t key, uint32_t position) : keyHash(key & 0xFFFFFFFFF), position(position) {}

        uint64_t keyHash;
        uint32_t position;
    };

    static constexpr size_t INDEX_SIZE = SIZE * 4;

public:
    class IterBase
    {
    public:
        typedef std::pair<KeyT, T> value_type;
        IterBase(ElementEntry* entries, uint32_t pos, uint32_t endPos) : _elements(entries), _pos(pos), _end(endPos) {}
        IterBase(const IterBase& it) : _elements(it._elements), _pos(it._pos), _end(it._end) {}

        IterBase& operator++()
        {
            if (_pos == _end)
            {
                return *this; // cannot advance a logical end iterator
            }

            ++_pos;
            while (_pos != _end && !_elements[_pos].committed)
            {
                ++_pos;
            }

            return *this;
        }

        value_type& operator*() { return _elements[_pos].keyValue; }
        value_type* operator->() { return &_elements[_pos].keyValue; }
        const value_type& operator*() const { return _elements[_pos].keyValue; }
        const value_type* operator->() const { return &_elements[_pos].keyValue; }
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
        ElementEntry* _elements;
        uint32_t _pos;
        uint32_t _end;
    };

    typedef const IterBase const_iterator;
    typedef IterBase iterator;
    typedef std::pair<KeyT, T> value_type;

    explicit Map() : _end(SIZE), _maxSpread(1), _count(0) {}

    std::pair<iterator, bool> add(const KeyT& key, const T& value)
    {
        auto keyHash = utils::hash<KeyT>{}(key);

        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            const auto& indexEntry = _index[indexPosition(keyHash, i)];
            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                return std::make_pair(end(), false); // no duplets
            }
        }

        const uint32_t freePos = findFreePosition();
        if (freePos == 0)
        {
            return std::make_pair(end(), false);
        }

        for (uint32_t i = 0; i < _index.size(); ++i)
        {
            updateSpread(i);
            auto& indexEntry = _index[indexPosition(keyHash, i)];
            if (indexEntry.position)
            {
                continue;
            }

            auto& elementEntry = _elements[freePos - 1];
            indexEntry.position = freePos;
            indexEntry.keyHash = keyHash;
            elementEntry.keyValue.second = value;
            elementEntry.keyValue.first = key;
            elementEntry.committed = true;
            ++_count;
            return std::make_pair(iterator(_elements.data(), freePos - 1, _end), true);
        }

        return std::make_pair(end(), false); // full
    }

    std::pair<iterator, bool> emplace(const KeyT& key, const T& value) { return add(key, value); }

    bool erase(const KeyT& key)
    {
        auto keyHash = utils::hash<KeyT>{}(key);
        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            auto& indexEntry = _index[indexPosition(keyHash, i)];
            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                _elements[indexEntry.position - 1] = ElementEntry();
                indexEntry = IndexEntry();
                --_count;
                return true;
            }
        }
        return false;
    }

    const T& operator[](const KeyT& key) const
    {
        auto keyHash = utils::hash<KeyT>{}(key);
        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            const auto& indexEntry = _index[indexPosition(keyHash, i)];
            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                return _elements[indexEntry.position - 1].keyValue.second;
            }
        }

        return _emptyObject;
    }

    T& operator[](const KeyT& key)
    {
        auto keyHash = utils::hash<KeyT>{}(key);
        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            const auto& indexEntry = _index[indexPosition(keyHash, i)];
            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                return _elements[indexEntry.position - 1].keyValue.second;
            }
        }

        if (add(key, T()).second)
        {
            return (*this)[key];
        }
        return _emptyObject;
    }

    iterator find(const KeyT& key)
    {
        const auto& map = *this;
        return iterator(map.find(key));
    }

    const_iterator find(const KeyT& key) const
    {
        auto keyHash = utils::hash<KeyT>{}(key);
        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            const auto& indexEntry = _index[indexPosition(keyHash, i)];

            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                return const_iterator(const_cast<ElementEntry*>(_elements.data()), indexEntry.position - 1, _end);
            }
        }
        return end();
    }

    bool contains(const KeyT& key) const
    {
        auto keyHash = utils::hash<KeyT>{}(key);
        for (uint32_t i = 0; i < _maxSpread; ++i)
        {
            const auto& indexEntry = _index[indexPosition(keyHash, i)];

            if (indexEntry.keyHash == keyHash && indexEntry.position)
            {
                return true;
            }
        }
        return false;
    }

    size_t capacity() const { return _elements.size(); }
    size_t size() const { return _count; }
    bool empty() const { return _count == 0; }

    void clear()
    {
        _nextFreeEntry = 0;
        _count = 0;
        _maxSpread = 1;
        for (size_t i = 0; i < SIZE; ++i)
        {
            if (_elements[i].committed)
            {
                _elements[i] = ElementEntry();
            }
        }
        for (size_t i = 0; i < INDEX_SIZE; ++i)
        {
            _index[i] = IndexEntry();
        }
    }

    const_iterator cbegin() const
    {
        uint32_t first = 0;
        while (first != _end && !_elements[first].committed)
        {
            ++first;
        }
        return const_iterator(const_cast<ElementEntry*>(_elements.data()), first, _end);
    }

    const_iterator cend() const { return const_iterator(const_cast<ElementEntry*>(_elements.data()), _end, _end); }

    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator begin() { return iterator(cbegin()); }
    iterator end() { return iterator(_elements.data(), _end, _end); }

    PointerType getItem(const KeyT& key)
    {
        auto it = find(key);
        if (it != end())
        {
            return detail::pointerOf(it->second);
        }
        return nullptr;
    }

    const PointerType getItem(const KeyT& key) const
    {
        return const_cast<Map<KeyT, T, SIZE>&>(*this).getItem(key);
    }

private:
    uint32_t indexPosition(uint64_t hashValue, uint32_t offset) const { return (hashValue + offset) % _index.size(); }

    void updateSpread(uint32_t i)
    {
        if (i >= _maxSpread)
        {
            _maxSpread = i + 1;
        }
    }

    uint32_t findFreePosition()
    {
        for (size_t i = 0; i < _index.size(); ++i)
        {
            if (!_elements[_nextFreeEntry].committed)
            {
                const auto freePosition = _nextFreeEntry + 1;
                _nextFreeEntry = (_nextFreeEntry + 1) % SIZE;
                return freePosition;
            }
            _nextFreeEntry = (_nextFreeEntry + 1) % SIZE;
        }

        return 0;
    }

    std::array<IndexEntry, INDEX_SIZE> _index;
    std::array<ElementEntry, SIZE> _elements;
    const uint32_t _end;
    uint32_t _nextFreeEntry = 0;
    T _emptyObject;
    uint32_t _maxSpread;
    uint32_t _count = 0;
};
} // namespace memory
