#include "MpmcHashmap.h"

namespace concurrency
{
MurmurHashIndex::MurmurHashIndex(size_t elementCount) : _index(elementCount), _maxSpread(elementCount == 0 ? 0 : 1)
{
    assert(memory::isAligned<uint64_t>(&_index[0])); // must be atomically writable
    assert(memory::isAligned<uint64_t>(&_index[0].keyValue)); // must be atomically writable
    static_assert(sizeof(KeyValue) == 8, "Index entry must be 8 bytes on your platform to be atomically writable");
    static_assert(sizeof(Entry) == 16, "Index entry should be 16B to reduce cacheline contention");
    std::memset(_index.data(), 0, sizeof(Entry) * _index.size());
}

void MurmurHashIndex::reInitialize()
{
    std::memset(_index.data(), 0, sizeof(Entry) * _index.size());
    _maxSpread = _index.size() == 0 ? 0 : 1;
}

bool MurmurHashIndex::add(uint64_t key, uint32_t value)
{
    assert(value != 0);
    key &= KEY_MASK;

    const uint64_t start = key;
    const auto spread = _maxSpread.load(std::memory_order::memory_order_acquire);
    for (uint32_t i = 0; i < spread; ++i)
    {
        const uint32_t pos = position(start, i);
        const auto entry = _index[pos].keyValue.load(std::memory_order::memory_order_acquire);
        if (entry.key == key && entry.value != 0)
        {
            return false; // no duplets
        }
    }

    const KeyValue item(key, value);
    for (uint32_t i = 0; i < _index.size(); ++i)
    {
        const uint32_t pos = position(start, i);
        KeyValue expected;
        updateSpread(i);
        if (_index[pos].keyValue.compare_exchange_strong(expected, item))
        {
            return true;
        }
        else if (expected.key == key)
        {
            return false; // no duplets allowed
        }
    }

    return false; // full
}

inline void MurmurHashIndex::updateSpread(uint32_t i)
{
    if (i < 1)
    {
        return;
    }

    for (uint32_t expected = _maxSpread.load(); i + 1 > expected;)
    {
        if (_maxSpread.compare_exchange_strong(expected, i + 1))
        {
            return;
        }
        // expected will now hold the actual maxSpread value
    }
}

// return value used to sync erase in hashmap
bool MurmurHashIndex::remove(uint64_t key)
{
    key &= KEY_MASK;
    const auto start = key;
    const auto spread = _maxSpread.load(std::memory_order::memory_order_acquire);
    for (uint32_t i = 0; i < spread; ++i)
    {
        const uint32_t pos = position(start, i);
        auto entry = _index[pos].keyValue.load(std::memory_order::memory_order_acquire);
        if (entry.key == key && entry.value != 0)
        {
            KeyValue empty;
            if (!_index[pos].keyValue.compare_exchange_strong(entry, empty))
            {
                empty.value = 99;
                return false;
            }
            assert(entry.key == key && entry.value != 0);
            return true;
        }
    }
    return false;
}

// position set to 0 when nothing more to find
// returns index of next item to try
uint32_t MurmurHashIndex::removeNext(uint32_t index, uint32_t& position)
{
    for (auto i = index; i < _index.size(); ++i)
    {
        auto content = _index[i].keyValue.load();
        if (content.value == 0)
        {
            continue;
        }

        KeyValue empty;
        if (_index[i].keyValue.compare_exchange_strong(content, empty))
        {
            position = content.value;
            return i + 1;
        }
    }

    position = 0;
    return _index.size();
}

bool MurmurHashIndex::get(uint64_t key, uint32_t& value) const
{
    key &= KEY_MASK;
    const auto start = key;
    const auto spread = _maxSpread.load(std::memory_order::memory_order_acquire);
    for (uint32_t i = 0; i < spread; ++i)
    {
        const uint32_t pos = position(start, i);
        const auto entry = _index[pos].keyValue.load(std::memory_order::memory_order_acquire);
        assert(entry.key == 0 || entry.value != 0);
        if (entry.key == key && entry.value != 0)
        {
            value = entry.value;
            return true;
        }
    }
    return false;
}

bool MurmurHashIndex::containsKey(uint64_t key) const
{
    key &= KEY_MASK;
    const auto start = key;
    const auto spread = _maxSpread.load(std::memory_order::memory_order_acquire);
    for (uint32_t i = 0; i < spread; ++i)
    {
        const uint32_t pos = position(start, i);
        const auto entry = _index[pos].keyValue.load(std::memory_order::memory_order_relaxed);

        if (entry.key == key && entry.value != 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace concurrency
