#pragma once

#include "utils/Optional.h"
#include "utils/StdExtensions.h"
#include <assert.h>
#include <cstdint>
#include <inttypes.h>
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
    uint32_t key[SIZE];

    TJsonPathCache() { memset(key, 0, sizeof(key)); }
    void put(const char* path, size_t pathLength, const char* in, const char* out)
    {
        const auto hash = utils::hash<char*>{}(path, pathLength);
        const size_t bin = hash % SIZE;
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
        const auto hash = utils::hash<char*>{}(path, pathLength);
        const size_t bin = hash % SIZE;
        if (key[bin] == hash)
        {
            outCursorIn = cursorIn[bin];
            outCursorOut = cursorOut[bin];
            return true;
        }
        return false;
    }
};

using JsonPathCache = TJsonPathCache<64>;

class SimpleJsonArray;

struct JsonToken
{
    JsonToken() {}
    explicit JsonToken(const char* start) : begin(start), end(nullptr) {}
    JsonToken(const char* start, const char* end) : begin(start), end(end) {}

    const char* begin = nullptr;
    const char* end = nullptr;

    bool isValid() const { return begin && end && end > begin; }
    size_t size() const { return isValid() ? static_cast<size_t>(end - begin) : 0; }

    void trim();

    bool isColon() const { return size() > 0 && *begin == ':'; }
    bool isComma() const { return size() > 0 && *begin == ','; }
    bool isString() const { return size() > 1 && *begin == '"'; }
    bool isObject() const { return size() > 1 && *begin == '{'; }
    bool isArray() const { return size() > 1 && *begin == '['; }
    bool isBoolean() const
    {
        return (size() >= 4 && 0 == std::strncmp("true", begin, 4)) ||
            (size() >= 5 && 0 == std::strncmp("false", begin, 5));
    }

    bool isNull() const { return size() >= 4 && 0 == std::strncmp("null", begin, 4); }
    bool isNumber() const { return size() > 0 && (*begin == '-' || std::isdigit(*begin)); }
};

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
    static SimpleJson create(const char* begin, size_t length) { return create(begin, begin + length); }
    static SimpleJson create(const char* begin, const char* end);

    SimpleJson() : _type(Type::None) {}
    SimpleJson(const SimpleJson& ref)
    {
        _item = ref._item;
        _type = ref._type;
    }

    SimpleJson& operator=(const SimpleJson& ref)
    {
        _item = ref._item;
        _type = ref._type;
        return *this;
    }

    Type getType() const { return _type; }
    SimpleJson find(const char* const path) const;
    SimpleJson operator[](const char* const path) const { return find(path); }
    bool exists(const char* path) const { return !find(path).isNone(); };

    Optional<int64_t> getInt() const;

    template <typename T>
    T getInt(T defaultValue) const
    {
        if (Type::Integer != _type)
        {
            return defaultValue;
        }
        char _buffer[33];
        std::strncpy(_buffer, _item.begin, size());
        _buffer[size()] = 0;
        if (_buffer[0] == '-')
        {
            __int64_t out;
            return (1 == sscanf(_buffer, "%" SCNd64, &out)) ? static_cast<T>(out) : defaultValue;
        }
        else
        {
            uint64_t out;
            return (1 == sscanf(_buffer, "%" SCNu64, &out)) ? static_cast<T>(out) : defaultValue;
        }
    }

    Optional<double> getFloat() const;
    double getFloat(double defaultValue) const;

    bool getString(const char*& out, size_t& outLen) const;

    template <size_t N>
    bool getString(char (&target)[N]) const
    {
        const char* s;
        size_t len;
        if (getString(s, len) && len < N)
        {
            std::strncpy(target, s, len);
            target[len] = 0;
            return true;
        }
        target[0] = 0;
        return false;
    }

    Optional<bool> getBool() const;
    SimpleJsonArray getArray() const;

    bool isNone() const { return _type == Type::None; }
    size_t size() const { return _item.size(); }

    static const SimpleJson SimpleJsonNone;

private:
    static SimpleJson findInCache(const char* const path, const size_t pathLength, const JsonPathCache& cache);
    SimpleJson(const char* begin, const char* end);

    SimpleJson findInternal(const char* const path,
        char* const cachedPath,
        size_t cachedPathSize,
        JsonPathCache& cache) const;

    SimpleJson findProperty(const char* start, const char* const name, const size_t nameLen) const;

    void assessType();

    JsonToken _item;
    Type _type;
    mutable JsonPathCache _cache;
};

class SimpleJsonArray
{
public:
    class IterBase
    {
    public:
        IterBase(const char* start, const char* arrayEnd);
        IterBase(const IterBase& it);

        IterBase& operator++();
        SimpleJson& operator*() { return _element; }
        SimpleJson* operator->() { return &_element; }
        const SimpleJson& operator*() const { return _element; }
        const SimpleJson* operator->() const { return &_element; }
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
        bool isEnd() const { return _pos == _arrayEnd; }
        const char* _pos;
        const char* _arrayEnd;
        mutable SimpleJson _element;
    };

    typedef const IterBase const_iterator;
    typedef IterBase iterator;

    SimpleJsonArray(const char* start, const char* end) : _item(start, end) { _item.trim(); }

    size_t size() const { return _item.size(); }
    size_t count() const;

    const_iterator cbegin() const { return const_iterator(_item.begin, _item.end); }
    const_iterator cend() const { return const_iterator(_item.end, _item.end); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }

    SimpleJson front() { return *begin(); }
    SimpleJson front() const { return *begin(); }

    bool empty() const { return _item.begin == _item.end; }
    bool isNull() const { return _item.begin == nullptr; }

private:
    JsonToken _item;
};
}; // namespace utils
