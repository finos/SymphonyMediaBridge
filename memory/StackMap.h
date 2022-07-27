#pragma once
#include "utils/StdExtensions.h"
#include <array>
#include <cinttypes>
#include <cstring>

namespace memory
{

template <typename KeyT, typename T, uint32_t SIZE>
class StackMap
{
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

    explicit StackMap() : _end(SIZE), _maxSpread(1), _count(0)
    {
        static_assert((SIZE & (SIZE - 1)) == 0, "StackMap size must be power of 2"); // must be power of two
    }

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

private:
    uint32_t indexPosition(uint64_t hashValue, uint32_t offset) const
    {
        return (hashValue + offset) & (_index.size() - 1);
    }

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

    std::array<IndexEntry, SIZE * 4> _index;
    std::array<ElementEntry, SIZE> _elements;
    const uint32_t _end;
    uint32_t _nextFreeEntry = 0;
    T _emptyObject;
    uint32_t _maxSpread;
    uint32_t _count = 0;
};
} // namespace memory
