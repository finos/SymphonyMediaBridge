#pragma once

#include <assert.h>
#include <string>
#include <vector>

namespace utils
{

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
    SimpleJson find(const std::string& path) const;

    bool getValue(int64_t& out) const;
    bool getValue(double& out) const;
    bool getValue(std::string& out) const;
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

    const char* findMatchEnd(const char* start, const std::string& match) const;
    SimpleJson findProperty(const char* start, const std::string& name) const;

    const char* eatDigits(const char* start) const;
    const char* eatWhiteSpaces(const char* start) const;
    const char* findPropertyEnd(const char* start) const;
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
};

}; // namespace utils