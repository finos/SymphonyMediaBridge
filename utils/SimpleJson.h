#pragma once

#include "utils/Optional.h"
#include <assert.h>
#include <cstdint>
#include <memory>
#include <stdio.h>

#define SIMPLE_JSON_CACHE_DIAG 0

namespace utils
{

template <size_t SIZE>
struct TJsonPathCache
{
    const char* cursorIn[SIZE];
    const char* cursorOut[SIZE];
    int32_t key[SIZE];
    TJsonPathCache() { memset(key, 0, sizeof(key)); }
    void put(const char* path, size_t pathLength, const char* in, const char* out)
    {
        auto hash = fnvHash(path, pathLength);
        size_t bin = hash % SIZE;
#if SIMPLE_JSON_CACHE_DIAG
        if (key[bin] != 0 && key[bin] != hash)
        {
            printf("\n Cache collision!\n");
        }
#endif
        key[bin] = hash;
        cursorIn[bin] = in;
        cursorOut[bin] = out;
    }
    bool get(const char* path, size_t pathLength, const char*& outCursorIn, const char*& outCursorOut) const
    {
        auto hash = fnvHash(path, pathLength);
        size_t bin = hash % SIZE;
        if (key[bin] == hash)
        {
            outCursorIn = cursorIn[bin];
            outCursorOut = cursorOut[bin];
            return true;
        }
        return false;
    }

private:
    static int32_t fnvHash(const char* str, size_t len);
};

using JsonPathCache = TJsonPathCache<64>;

template <size_t MAX_SIZE>
class TSimpleJsonArray;

using SimpleJsonArray = TSimpleJsonArray<100>;

class SimpleJson
{
    friend SimpleJsonArray;

public:
    enum Type
    {
        None,
        Boolean,
        Null,
        String,
        Integer,
        Float,
        Object,
        Array
    };
    static SimpleJson create(const char* cursorIn, size_t length);
    static const SimpleJson SimpleJsonNone;

    SimpleJson() : _cursorIn(nullptr), _cursorOut(nullptr), _type(Type::None) {}
    SimpleJson(const SimpleJson& ref)
    {
        _cursorIn = ref._cursorIn;
        _cursorOut = ref._cursorOut;
        _type = ref._type;
    }
    SimpleJson& operator=(const SimpleJson& ref)
    {
        _cursorIn = ref._cursorIn;
        _cursorOut = ref._cursorOut;
        _type = ref._type;
        return *this;
    }

    Type getType() const { return _type; }
    SimpleJson find(const char* const path);

    Optional<int64_t> getIntValue() const;
    Optional<double> getFloatValue() const;
    bool getStringValue(const char*& out, size_t& outLen) const;
    Optional<bool> getBoolValue() const;
    Optional<SimpleJsonArray> getArrayValue() const;

private:
    static SimpleJson create(const char* cursorIn, const char* cursorOut);
    static SimpleJson createJsonNone() { return SimpleJson(nullptr, 0); }
    static SimpleJson findInCache(const char* const path, const size_t pathLength, const JsonPathCache& cache);
    SimpleJson(const char* json, size_t length) : _cursorIn(json), _cursorOut(_cursorIn + length - 1)
    {
        if (json)
        {
            validateFast();
        }
        else
        {
            _type = Type::None;
        }
    }

    template <char OPEN_CHAR, char CLOSE_CHAR>
    const char* findEnd(const char* start) const;

    SimpleJson findInternal(const char* const path,
        char* const cachedPath,
        size_t cachedPathSize,
        JsonPathCache& cache);
    const char* findMatchEnd(const char* start, const char* const match, size_t matchLen) const;
    SimpleJson findProperty(const char* start, const char* const name, const size_t nameLen) const;

    const char* eatDigits(const char* start) const;
    const char* eatWhiteSpaces(const char* start) const;
    const char* findValueEnd(const char* start) const;
    const char* findStringEnd(const char* start) const;
    const char* findBooleanEnd(const char* start) const;
    const char* findNumberEnd(const char* start) const;
    const char* findNullEnd(const char* start) const;
    const char* findObjectEnd(const char* start) const { return findEnd<'{', '}'>(start); };
    const char* findArrayEnd(const char* start) const { return findEnd<'[', ']'>(start); };

    void validateFast();
    Type acquirePrimitiveType();
    size_t size() const { return _cursorOut - _cursorIn + 1; }

private:
    const char* _cursorIn;
    const char* _cursorOut;
    Type _type;
    JsonPathCache _cache;
};

template <size_t MAX_SIZE>
class TSimpleJsonArray
{
public:
    struct ArrayEntry
    {
        const char* cursorIn;
        const char* cursorOut;
        SimpleJson toJson() const { return SimpleJson::create(cursorIn, cursorOut); }
    };
    class IterBase
    {
    public:
        IterBase(ArrayEntry* entries, size_t pos, size_t endPos) : _elements(entries), _pos(pos), _end(endPos) {}
        IterBase(const IterBase& it) : _elements(it._elements), _pos(it._pos), _end(it._end) {}

        IterBase& operator++()
        {
            if (_pos == _end)
            {
                return *this; // cannot advance a logical end iterator
            }
            ++_pos;
            return *this;
        }

        ArrayEntry& operator*() { return _elements[_pos]; }
        ArrayEntry* operator->() { return &_elements[_pos]; }
        const ArrayEntry& operator*() const { return _elements[_pos]; }
        const ArrayEntry* operator->() const { return &_elements[_pos]; }
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
        ArrayEntry* _elements;
        size_t _pos;
        size_t _end;
    };

    typedef const IterBase const_iterator;
    typedef IterBase iterator;

    void clear() { _size = 0; }
    size_t capacity() const { return MAX_SIZE; }
    size_t size() const { return _size; }

    const_iterator cbegin() const { return const_iterator(const_cast<ArrayEntry*>(&_entries[0]), 0, _size); }
    const_iterator cend() const { return const_iterator(const_cast<ArrayEntry*>(&_entries[0]), _size + 1, _size + 1); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator cbegin() { return iterator(_entries, 0, _size); }
    iterator cend() { return iterator(_entries, _size + 1, _size + 1); }

    void push_back(const char* cursorIn, const char* cursorOut)
    {
        if (_size + 1 < MAX_SIZE)
        {
            _size++;
            _entries[_size - 1].cursorIn = cursorIn;
            _entries[_size - 1].cursorOut = cursorOut;
        }
    }

    ArrayEntry& operator[](size_t i)
    {
        assert(i < _size);
        return _entries[i];
    }

private:
    ArrayEntry _entries[MAX_SIZE];
    size_t _size = 0;
};

}; // namespace utils