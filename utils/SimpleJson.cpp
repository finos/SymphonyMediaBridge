#include "SimpleJson.h"
#include "StringTokenizer.h"
#include <inttypes.h>
#include <math.h>

namespace utils
{
template <size_t SIZE>
int32_t TJsonPathCache<SIZE>::fnvHash(const char* str, size_t len)
{
    static constexpr int32_t FNV1_32_INIT = 0x811c9dc5;
    unsigned char* s = (unsigned char*)str;
    int32_t hval = FNV1_32_INIT;

    while (len--)
    {
        hval += (hval << 1) + (hval << 4) + (hval << 7) + (hval << 8) + (hval << 24);
        hval ^= (int32_t)*s++;
    }
    return hval;
}

const SimpleJson SimpleJson::SimpleJsonNone = SimpleJson::createJsonNone();

SimpleJson SimpleJson::create(const char* cursorIn, size_t length)
{
    if (!cursorIn || 0 == length)
    {
        return SimpleJsonNone;
    }
    return SimpleJson(cursorIn, length);
}

SimpleJson SimpleJson::create(const char* cursorIn, const char* cursorOut)
{
    if (!cursorIn || !cursorOut || cursorIn > cursorOut)
    {
        return SimpleJsonNone;
    }

    return SimpleJson(cursorIn, cursorOut - cursorIn + 1);
}

void SimpleJson::validateFast()
{
    if (!_cursorIn || !_cursorOut)
    {
        _type = Type::None;
        return;
    }
    while (_cursorIn != _cursorOut && std::isspace(*_cursorIn))
    {
        _cursorIn++;
    }
    while (_cursorOut != _cursorIn && std::isspace(*_cursorOut))
    {
        _cursorOut--;
    }
    if (*_cursorIn == '{' && *_cursorOut == '}')
    {
        _type = Type::Object;
    }
    else if (*_cursorIn == '[' && *_cursorOut == ']')
    {
        _type = Type::Array;
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
    char _buffer[33];
    if (cursor == _cursorOut && size() < sizeof(_buffer))
    {
        strncpy(_buffer, _cursorIn, size());
        _buffer[size()] = 0;
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
        {
            return Type::Integer;
        }
        if (!readInt && readFloat)
        {
            return Type::Float;
        }
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
    {
        return end;
    }

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
    {
        return end;
    }

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

const char* SimpleJson::findMatchEnd(const char* start, const char* const match, size_t matchLen) const
{
    if (start + matchLen - 1 > _cursorOut)
    {
        return nullptr;
    }
    return strncmp(start, match, matchLen) ? nullptr : start + matchLen - 1;
}

const char* SimpleJson::findBooleanEnd(const char* start) const
{
    auto result = findMatchEnd(start, "true", 4);
    return result ? result : findMatchEnd(start, "false", 5);
}

const char* SimpleJson::findNullEnd(const char* start) const
{
    return findMatchEnd(start, "null", 4);
}

// Finds the end of the number.
// start should point at opening - or digit character.
// Returns position of last number-belonging character;
// NOTE: naive implementation!
const char* SimpleJson::findNumberEnd(const char* start) const
{
    if (!start || ('-' != *start && !std::isdigit(*start)))
    {
        return nullptr;
    }

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
    {
        return nullptr;
    }

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
    {
        return nullptr;
    }

    auto cursor = start - 1;
    while (++cursor <= _cursorOut)
    {
        if (!std::isspace(*cursor))
            return cursor;
    }
    return nullptr;
}

// Finds the end of the property.
// start should point at the the first char after :.
// Returns position of the last char beloning to the value.
// including " or }.
const char* SimpleJson::findValueEnd(const char* start) const
{
    auto cursor = start;
    if (!start)
    {
        return nullptr;
    }

    cursor = eatWhiteSpaces(cursor);
    if (!cursor)
    {
        return nullptr;
    }
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
SimpleJson SimpleJson::findProperty(const char* start, const char* const name, const size_t nameLen) const
{
    auto cursor = start;
    if (!nameLen || !start || ('"' != *start && !std::isspace(*start)))
    {
        return SimpleJsonNone;
    }

    while (cursor <= _cursorOut)
    {
        cursor = eatWhiteSpaces(cursor);
        if (!cursor)
        {
            return SimpleJsonNone;
        }
        if ('"' != *cursor)
        {
            return SimpleJsonNone;
        }
        // Read property name.
        auto propName = cursor;
        auto propNameEnd = findStringEnd(cursor);
        if (!propNameEnd)
        {
            return SimpleJsonNone;
        }
        cursor = eatWhiteSpaces(propNameEnd + 1);
        if (!cursor)
        {
            return SimpleJsonNone;
        }
        if (':' != *cursor)
        {
            return SimpleJsonNone;
        }
        // Read property value.
        auto valueStart = cursor + 1;
        if (valueStart >= _cursorOut)
        {
            return SimpleJsonNone;
        }
        auto valueEnd = findValueEnd(cursor + 1);
        if (!valueEnd)
        {
            return SimpleJsonNone;
        }
        // Check whether it is our property.
        if (!strncmp(propName + 1, name, nameLen))
        {
            return SimpleJson::create(valueStart, valueEnd);
        }
        // Go for the next one.
        cursor = eatWhiteSpaces(valueEnd + 1);
        if (!cursor)
        {
            return SimpleJsonNone;
        }
        if (',' != *cursor)
        {
            return SimpleJsonNone;
        }
        cursor++;
    }

    return SimpleJsonNone;
}

SimpleJson SimpleJson::findInCache(const char* const path, const size_t pathLength, const JsonPathCache& cache)
{
    const char* in;
    const char* out;
    if (cache.get(path, pathLength, in, out))
    {
        return SimpleJson::create(in, out);
    }
    return SimpleJsonNone;
}

SimpleJson SimpleJson::find(const char* const path)
{
    auto node = findInCache(path, strlen(path), _cache);
    if (node.getType() != Type::None)
    {
#if SIMPLE_JSON_CACHE_DIAG
        printf("\n Found in cache for path %s\n", path);
#endif
        return node;
    }
    char cachedPath[1024];
    cachedPath[0] = 0;
    return findInternal(path, cachedPath, 0, _cache);
}

SimpleJson SimpleJson::findInternal(const char* const path,
    char* const cachedPath,
    size_t cachedPathSize,
    JsonPathCache& cache)
{
    auto token = StringTokenizer::tokenize(path, strlen(path), '.');
    if (token.empty() || Type::Object != _type)
    {
        return SimpleJsonNone;
    }

    if (cachedPathSize)
    {
        strcat(cachedPath, ".");
        cachedPathSize++;
    }
    strncpy(cachedPath + cachedPathSize, token.start, token.length);
    cachedPathSize += token.length;
    cachedPath[cachedPathSize] = 0;

    auto property = findInCache(cachedPath, cachedPathSize, cache);
    if (property.getType() == Type::None)
    {
        property = findProperty(_cursorIn + 1, token.start, token.length);
        cache.put(cachedPath, cachedPathSize, property._cursorIn, property._cursorOut);
#if SIMPLE_JSON_CACHE_DIAG
        printf("\n Put in cache path %s\n", cachedPath);
#endif
    }
#if SIMPLE_JSON_CACHE_DIAG
    else
    {
        printf("\n Found in cache for path %s\n", cachedPath);
    }
#endif

    if (property.getType() == SimpleJson::Type::None || !token.next)
    {
        return property;
    }

    return (property.getType() == SimpleJson::Type::Object)
        ? property.findInternal(token.next, cachedPath, cachedPathSize, cache)
        : SimpleJsonNone;
}

Optional<int64_t> SimpleJson::getIntValue() const
{
    if (Type::Integer != _type)
    {
        return Optional<int64_t>();
    }
    char _buffer[33];
    strncpy(_buffer, _cursorIn, size());
    int64_t out;
    return (1 == sscanf(_buffer, "%" SCNd64, &out)) ? Optional<int64_t>(out) : Optional<int64_t>();
}

Optional<double> SimpleJson::getFloatValue() const
{
    if (Type::Float != _type)
    {
        return Optional<double>();
    }
    char _buffer[33];
    strncpy(_buffer, _cursorIn, size());
    double out;
    return (1 == sscanf(_buffer, "%lf", &out)) ? Optional<double>(out) : Optional<double>();
}

bool SimpleJson::getStringValue(const char*& out, size_t& outLen) const
{
    if (Type::String != _type || size() < 2)
    {
        return false;
    }
    out = _cursorIn + 1;
    outLen = size() - 2;
    return true;
}

Optional<bool> SimpleJson::getBoolValue() const
{
    if (Type::Boolean != _type)
    {
        return Optional<bool>();
    }
    if (size() == 4 && !strncmp(_cursorIn, "true", 4))
    {
        return Optional<bool>(true);
    }
    if (size() == 5 && !strncmp(_cursorIn, "false", 5))
    {
        return Optional<bool>(false);
    }
    return Optional<bool>();
}

Optional<SimpleJsonArray> SimpleJson::getArrayValue() const
{
    if (Type::Array != _type || '[' != *_cursorIn)
    {
        return Optional<SimpleJsonArray>();
    }
    SimpleJsonArray out;
    out.clear();
    auto cursor = _cursorIn + 1;
    auto end = cursor;
    while (cursor < _cursorOut)
    {
        end = findValueEnd(cursor);
        if (end)
        {
            out.push_back(cursor, end);
        }
        // Go for the next one
        cursor = eatWhiteSpaces(end + 1);
        if (cursor && ',' != *cursor)
        {
            cursor++;
        }
        cursor++;
    }
    return Optional<SimpleJsonArray>(out);
}

} // namespace utils