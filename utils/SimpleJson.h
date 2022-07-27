#pragma once

#include <assert.h>
#include <vector>

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
    size_t index = 0;
};

using JsonPathCache = TJsonPathCache<100>;

class SimpleJson
{
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

    bool getValue(int64_t& out) const;
    bool getValue(double& out) const;
    bool getStringValue(const char*& out, size_t& outLen) const;
    bool getValue(bool& out) const;
    bool getValue(std::vector<SimpleJson>& out) const;

    template <typename T>
    T valueOr(const T&& defaultValue) const
    {
        T outVal;
        return getValue(outVal) ? outVal : defaultValue;
    }

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

}; // namespace utils