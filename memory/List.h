#pragma once

#include "memory/PoolAllocator.h"
#include <cstddef>

namespace memory
{

template <typename T, size_t MAX_SIZE>
class List
{
    static_assert(std::is_trivially_copy_assignable<T>(), "Contained type in List must be trivially assignable");
    static_assert(std::is_trivially_constructible<T>(), "Contained type in List must be trivially constructible");

public:
    struct Entry
    {
        T _data;
        Entry* _next;
        Entry* _previous;
    };

    List() : _head(nullptr), _tail(nullptr), _entryAllocator(MAX_SIZE, "List") {}
    ~List()
    {
        while (_head)
        {
            auto item = _head;
            _head = item->_next;
            item->~Entry();
            _entryAllocator.free(item);
        }
    }

    bool pushToTail(const T& data)
    {
        if (!_head)
        {
            _head = reinterpret_cast<Entry*>(_entryAllocator.allocate());
            if (!_head)
            {
                return false;
            }

            _head->_next = nullptr;
            _head->_previous = nullptr;
            _head->_data = data;
            _tail = _head;
            return true;
        }

        auto newEntry = reinterpret_cast<Entry*>(_entryAllocator.allocate());
        if (!newEntry)
        {
            return false;
        }
        newEntry->_next = nullptr;
        newEntry->_previous = _tail;
        newEntry->_data = data;
        _tail->_next = newEntry;
        _tail = newEntry;

        return true;
    }

    bool remove(const T& data)
    {
        auto entry = _head;
        while (entry)
        {
            if (entry->_data == data)
            {
                if (entry->_previous)
                {
                    entry->_previous->_next = entry->_next;
                }
                else
                {
                    _head = entry->_next;
                }

                if (entry->_next)
                {
                    entry->_next->_previous = entry->_previous;
                }
                else
                {
                    _tail = entry->_previous;
                }

                _entryAllocator.free(entry);
                return true;
            }
            entry = entry->_next;
        }

        return false;
    }

    bool pushToHead(const T& data)
    {
        if (!_head)
        {
            return pushToTail(data);
        }

        auto newEntry = reinterpret_cast<Entry*>(_entryAllocator.allocate());
        if (!newEntry)
        {
            return false;
        }
        newEntry->_next = _head;
        newEntry->_previous = nullptr;
        newEntry->_data = data;
        _head->_previous = newEntry;
        _head = newEntry;

        return true;
    }

    bool popFromHead(T& outData)
    {
        if (!_head)
        {
            return false;
        }

        auto entry = _head;
        outData = entry->_data;

        if (entry->_previous)
        {
            entry->_previous->_next = entry->_next;
        }
        else
        {
            _head = entry->_next;
        }

        if (entry->_next)
        {
            entry->_next->_previous = entry->_previous;
        }
        else
        {
            _tail = entry->_previous;
        }

        _entryAllocator.free(entry);
        return true;
    }

    Entry* head() const { return _head; }
    Entry* tail() const { return _tail; }

private:
    Entry* _head;
    Entry* _tail;
    memory::PoolAllocator<sizeof(Entry)> _entryAllocator;
};

} // namespace memory
