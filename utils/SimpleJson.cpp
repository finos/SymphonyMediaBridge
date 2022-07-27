#include "SimpleJson.h"
#include "StringTokenizer.h"
#include <inttypes.h>
#include <math.h>

namespace utils
{

const SimpleJson SimpleJson::SimpleJsonNone = SimpleJson::createJsonNone();

SimpleJson SimpleJson::create(const char* cursorIn, size_t length)
{
    if (!cursorIn || 0 == length)
        return SimpleJsonNone;
    else
        return SimpleJson(cursorIn, length);
}

SimpleJson SimpleJson::create(const char* cursorIn, const char* cursorOut)
{
    if (!cursorIn || !cursorOut || cursorIn >= cursorOut)
        return SimpleJsonNone;
    else
        return SimpleJson(cursorIn, cursorOut - cursorIn + 1);
}

void SimpleJson::validateFast()
{
    while (_cursorIn != _cursorOut && std::isspace(*_cursorIn))
        _cursorIn++;
    while (_cursorOut != _cursorIn && std::isspace(*_cursorOut))
        _cursorOut--;
    if (*_cursorIn == '{' && *_cursorOut == '}')
    {
        _type = Type::Object;
    }
    else if (*_cursorIn == '[' && *_cursorOut == ']')
    {
        _type = Type::Array;
    }
    else if (_cursorIn == _cursorOut)
    {
        _type = Type::None;
    }
    else
    {
        _type = acquirePrimitiveType();
    }
}

SimpleJson::Type SimpleJson::acquirePrimitiveType()
{
    auto cursor = findStringEnd(_cursorIn);
    if (cursor == _cursorOut)
    {
        return Type::String;
    }
    cursor = findNullEnd(_cursorIn);
    if (cursor == _cursorOut)
    {
        return Type::Null;
    }
    cursor = findBooleanEnd(_cursorIn);
    if (cursor == _cursorOut)
    {
        return Type::Boolean;
    }
    cursor = findNumberEnd(_cursorIn);
    if (cursor == _cursorOut && size() < sizeof(_buffer))
    {
        strncpy(_buffer, _cursorIn, size());
        int64_t intVal;
        double floatVal, intPart;
        auto readInt = sscanf(_buffer, "%" SCNd64, &intVal);
        auto readFloat = sscanf(_buffer, "%lf", &floatVal);
        if (readInt && readFloat)
        {
            // Return Float if we have fractional part. Otherwise Integer should suffice.
            return modf(floatVal, &intPart) != 0 ? Type::Float : Type::Integer;
        }
        if (readInt && !readFloat)
            return Type::Integer;
        if (!readInt && readFloat)
            return Type::Float;
    }
    return Type::None;
}

// Finds the end of the string.
// start should point at opening " character.
// Returns position of the closing " character.
const char* SimpleJson::findStringEnd(const char* start) const
{
    const char* end = nullptr;
    if (!start || '"' != *start)
        return end;

    auto cursor = start;
    while (++cursor <= _cursorOut)
    {
        if ('\\' == *cursor)
        {
            cursor++;
            continue;
        }
        if ('"' == *cursor)
        {
            end = cursor;
            break;
        }
    }
    return end;
}

// Finds the end of the object.
// start should point at opening { character.
// Returns position of the closing } character.
template <char OPEN_CHAR, char CLOSE_CHAR>
const char* SimpleJson::findEnd(const char* start) const
{
    const char* end = nullptr;
    auto cursor = start;
    if (!start || OPEN_CHAR != *start)
        return end;

    int level = 1;
    while (++cursor < _cursorOut)
    {
        switch (*cursor)
        {
        case OPEN_CHAR:
            level++;
            continue;
        case CLOSE_CHAR:
            level--;
            break;
        case '"':
            cursor = findStringEnd(cursor);
            continue;
        }
        if (level == 0)
        {
            end = cursor;
            break;
        }
    }
    return end;
}

const char* SimpleJson::findMatchEnd(const char* start, const std::string& match) const
{
    if (start + match.length() - 1 > _cursorOut)
        return nullptr;
    return strncmp(start, match.c_str(), match.length()) ? nullptr : start + match.length() - 1;
}
const char* SimpleJson::findBooleanEnd(const char* start) const
{
    auto result = findMatchEnd(start, "true");
    return result ? result : findMatchEnd(start, "false");
}
const char* SimpleJson::findNullEnd(const char* start) const
{
    return findMatchEnd(start, "null");
}

// Finds the end of the number.
// start should point at opening - or digit character.
// Returns position of last number-belonging character;
// NOTE: naive implementation!
const char* SimpleJson::findNumberEnd(const char* start) const
{
    if (!start || ('-' != *start && !std::isdigit(*start)))
        return nullptr;

    auto cursor = start - 1;

    while (++cursor <= _cursorOut)
    {
        switch (*cursor)
        {
        case '-':
        case '.':
        case 'e':
        case 'E':
            continue;
        default:
            if (!std::isdigit(*cursor))
                return cursor - 1;
        }
    }

    return cursor - 1;
}

const char* SimpleJson::eatDigits(const char* start) const
{
    if (!start)
        return nullptr;

    auto cursor = start - 1;
    while (++cursor <= _cursorOut)
    {
        if (!std::isdigit(*cursor))
            return cursor;
    }
    return nullptr;
}

const char* SimpleJson::eatWhiteSpaces(const char* start) const
{
    if (!start)
        return nullptr;

    auto cursor = start - 1;
    while (++cursor <= _cursorOut)
    {
        if (!std::isspace(*cursor))
            return cursor;
    }
    return nullptr;
}

// Finds the end of the property.
// start should point at the : character.
// Returns position of the closing one of ,]} character.
const char* SimpleJson::findPropertyEnd(const char* start) const
{
    const char* end = nullptr;
    auto cursor = start;
    if (!start || ':' != *start)
        return end;

    cursor = eatWhiteSpaces(cursor + 1);
    if (!cursor)
        return nullptr;
    switch (*cursor)
    {
    case '{':
        return findObjectEnd(cursor);
    case '[':
        return findArrayEnd(cursor);
    case '"':
        return findStringEnd(cursor);
    case 't':
    case 'f':
        return findBooleanEnd(cursor);
    case 'n':
        return findNullEnd(cursor);
    }

    return findNumberEnd(cursor);
}

// Finds the property on the current level.
// start should point at the " or whitespace character.
// Returns SimpleJson object of the property's value
SimpleJson SimpleJson::findProperty(const char* start, const std::string& name) const
{
    auto cursor = start;
    if (name.empty() || !start || ('"' != *start && !std::isspace(*start)))
        return SimpleJsonNone;

    while (cursor <= _cursorOut)
    {
        cursor = eatWhiteSpaces(cursor);
        if (!cursor)
            return SimpleJsonNone;
        if ('"' != *cursor)
            return SimpleJsonNone;
        // Read property name
        auto propName = cursor;
        auto propNameEnd = findStringEnd(cursor);
        if (!propNameEnd)
            return SimpleJsonNone;
        cursor = eatWhiteSpaces(propNameEnd + 1);
        if (!cursor)
            return SimpleJsonNone;
        if (':' != *cursor)
            return SimpleJsonNone;
        // Read property value
        auto valueStart = cursor + 1;
        if (valueStart >= _cursorOut)
            return SimpleJsonNone;
        auto valueEnd = findPropertyEnd(cursor);
        if (!valueEnd)
            return SimpleJsonNone;
        // Check whether it is our property
        if (!strncmp(propName + 1, name.c_str(), name.length()))
        {
            return SimpleJson::create(valueStart, valueEnd);
        }
        // Go for the next one
        cursor = eatWhiteSpaces(valueEnd + 1);
        if (!cursor)
            return SimpleJsonNone;
        if (',' != *cursor)
            return SimpleJsonNone;
        cursor++;
    }

    return SimpleJsonNone;
}

SimpleJson SimpleJson::find(const std::string& path) const
{
    auto token = StringTokenizer::tokenize(path.c_str(), path.length(), '.');
    if (token.empty() || Type::Object != _type)
        return SimpleJsonNone;

    auto property = findProperty(_cursorIn + 1, token.str());
    if (property.getType() == SimpleJson::Type::None || !token.next)
        return property;
    else
        return (property.getType() == SimpleJson::Type::Object) ? property.find(token.next) : SimpleJsonNone;
}

bool SimpleJson::getValue(int64_t& out)
{
    if (Type::Integer != _type)
        return false;
    strncpy(_buffer, _cursorIn, size());
    return 1 == sscanf(_buffer, "%" SCNd64, &out);
}

bool SimpleJson::getValue(double& out)
{
    if (Type::Float != _type)
        return false;
    strncpy(_buffer, _cursorIn, size());
    return 1 == sscanf(_buffer, "%lf", &out);
}

bool SimpleJson::getValue(std::string& out)
{
    if (Type::String != _type || size() < 2)
        return false;
    out = std::string(_cursorIn + 1, size() - 2);
    return true;
}

bool SimpleJson::getValue(bool& out)
{
    if (Type::Boolean != _type)
        return false;
    if (size() == 4 && !strncmp(_cursorIn, "true", 4))
    {
        out = true;
        return true;
    }
    if (size() == 5 && !strncmp(_cursorIn, "false", 5))
    {
        out = false;
        return true;
    }
    return false;
}

} // namespace utils