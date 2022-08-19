#include "SimpleJson.h"
#include "StringTokenizer.h"
#include <inttypes.h>
#include <math.h>

namespace utils
{
namespace Json
{
// Finds the end of the string.
// start should point at opening " character.
// Returns position after the closing " character.
JsonToken extractString(const JsonToken& item)
{
    if (!item.begin || '"' != *item.begin)
    {
        return JsonToken();
    }

    for (auto cursor = item.begin + 1; cursor != item.end; ++cursor)
    {
        if ('\\' == *cursor)
        {
            continue;
        }
        else if ('"' == *cursor)
        {
            return JsonToken(item.begin, cursor + 1);
        }
    }

    return JsonToken();
}

// Finds the end of the object.
// start should point at opening { character.
// Returns position of the closing } character.
template <char OPEN_CHAR, char CLOSE_CHAR>
JsonToken extractItem(const JsonToken& item)
{
    if (!item.begin || OPEN_CHAR != *item.begin)
    {
        return JsonToken();
    }

    int level = 0;
    for (auto cursor = item.begin; cursor != item.end;)
    {
        switch (*cursor)
        {
        case OPEN_CHAR:
            ++level;
            break;
        case CLOSE_CHAR:
            if (--level == 0)
            {
                return JsonToken(item.begin, cursor + 1);
            }
            break;
        case '"':
            auto stringItem = extractString(JsonToken(cursor, item.end));
            if (!stringItem.isValid())
            {
                return JsonToken();
            }
            cursor = stringItem.end;
            continue;
        }
        ++cursor;
    }

    return JsonToken();
}

JsonToken extractWord(const JsonToken& item)
{
    JsonToken result = item;
    for (auto cursor = item.begin; cursor != item.end; ++cursor)
    {
        if (*cursor == ',' || *cursor == '}' || std::isspace(*cursor))
        {
            result.end = cursor;
            return result;
        }
    }
    return item;
}

// Finds the end of the number.
// start should point at opening - or digit character.
// Returns position of last number-belonging character;
// NOTE: naive implementation!
JsonToken extractNumber(const JsonToken& item)
{
    if (!item.begin || ('-' != *item.begin && !std::isdigit(*item.begin)))
    {
        return JsonToken();
    }

    for (auto cursor = item.begin; cursor != item.end; ++cursor)
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
                return JsonToken(item.begin, cursor);
        }
    }

    return item;
}

JsonToken nextToken(const JsonToken& item)
{
    if (!item.begin)
    {
        return JsonToken();
    }

    auto cursor = item.begin;
    while (cursor != item.end && std::isspace(*cursor))
    {
        ++cursor;
    }

    if (cursor == item.end)
    {
        return JsonToken();
    }

    switch (*cursor)
    {
    case '{':
        return extractItem<'{', '}'>(JsonToken(cursor, item.end));
    case '[':
        return extractItem<'[', ']'>(JsonToken(cursor, item.end));
    case '"':
        return extractString(JsonToken(cursor, item.end));
    case 't':
    case 'f':
        return extractWord(JsonToken(cursor, item.end));
    case 'n':
        return extractWord(JsonToken(cursor, item.end));
    case ':':
        return JsonToken(cursor, cursor + 1);
    case ',':
        return JsonToken(cursor, cursor + 1);
    default:
        return extractNumber(JsonToken(cursor, item.end));
    }

    return JsonToken();
}
} // namespace Json

const SimpleJson SimpleJson::SimpleJsonNone = SimpleJson(nullptr, nullptr);

void JsonToken::trim()
{
    if (!begin || !end)
    {
        return;
    }

    while (begin != end && std::isspace(*begin))
    {
        ++begin;
    }
    while (end > begin && std::isspace(*(end - 1)))
    {
        --end;
    }
}

SimpleJson SimpleJson::create(const char* c_str)
{
    return create(c_str, strlen(c_str));
}

SimpleJson SimpleJson::create(const char* cursorIn, const char* cursorOut)
{
    if (!cursorIn || !cursorOut || cursorIn > cursorOut)
    {
        return SimpleJsonNone;
    }

    return SimpleJson(cursorIn, cursorOut);
}

SimpleJson::SimpleJson(const char* begin, const char* end) : _item(begin, end)
{
    if (begin)
    {
        assessType();
    }
    else
    {
        _type = Type::None;
    }
}

void SimpleJson::assessType()
{
    _type = Type::None;
    if (_item.isValid())
    {
        _item.trim();
    }

    if (!_item.isValid() || _item.size() == 0)
    {
        return;
    }

    if (_item.isObject())
    {
        _type = Type::Object;
    }
    else if (_item.isArray())
    {
        _type = Type::Array;
    }
    else if (_item.isString())
    {
        _type = Type::String;
    }
    else if (_item.isBoolean())
    {
        _type = Type::Boolean;
    }
    else if (_item.isNull())
    {
        _type = Type::Null;
    }
    else if (_item.isNumber())
    {
        for (auto c = _item.begin; c != _item.end; ++c)
        {
            if (*c == '.')
            {
                _type = Type::Float;
                return;
            }
            else if (*c == ',' || std::isspace(*c))
            {
                break;
            }
        }
        _type = Type::Integer;
    }
}

// Finds the property on the current level.
// start should point at the " or whitespace character.
// Returns SimpleJson object of the property's value
SimpleJson SimpleJson::findProperty(const char* start, const char* const name, const size_t nameLen) const
{
    if (!nameLen || !start || ('"' != *start && !std::isspace(*start)))
    {
        return SimpleJsonNone;
    }

    for (auto cursor = start; cursor < _item.end;)
    {
        auto propertyName = Json::nextToken(JsonToken(cursor, _item.end));
        if (!propertyName.isString())
        {
            return SimpleJsonNone;
        }

        cursor = propertyName.end;
        ++propertyName.begin;
        --propertyName.end;

        auto colon = Json::nextToken(JsonToken(cursor, _item.end));
        if (!colon.isColon())
        {
            return SimpleJsonNone;
        }

        auto value = Json::nextToken(JsonToken(colon.end, _item.end));

        if (propertyName.size() == nameLen && 0 == std::strncmp(propertyName.begin, name, nameLen))
        {
            return SimpleJson::create(value.begin, value.end);
        }

        auto delimiter = Json::nextToken(JsonToken(value.end, _item.end));
        if (!delimiter.isComma())
        {
            return SimpleJsonNone;
        }

        cursor = delimiter.end;
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

SimpleJson SimpleJson::find(const char* const path) const
{
    auto node = findInCache(path, std::strlen(path), _cache);
    if (!node.isNone())
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
    JsonPathCache& cache) const
{
    auto token = StringTokenizer::tokenize(path, std::strlen(path), '.');
    if (token.empty() || Type::Object != _type)
    {
        return SimpleJsonNone;
    }

    if (cachedPathSize)
    {
        std::strcat(cachedPath, ".");
        cachedPathSize++;
    }
    std::strncpy(cachedPath + cachedPathSize, token.start, token.length);
    cachedPathSize += token.length;
    cachedPath[cachedPathSize] = 0;

    auto property = findInCache(cachedPath, cachedPathSize, cache);
    if (property.isNone())
    {
        property = findProperty(_item.begin + 1, token.start, token.length);
        cache.put(cachedPath, cachedPathSize, property._item.begin, property._item.end);
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

    if (property.isNone() || !token.next)
    {
        return property;
    }

    return (property.getType() == SimpleJson::Type::Object)
        ? property.findInternal(token.next, cachedPath, cachedPathSize, cache)
        : SimpleJsonNone;
}

Optional<int64_t> SimpleJson::getInt() const
{
    if (Type::Integer != _type)
    {
        return Optional<int64_t>();
    }
    char _buffer[33];
    std::strncpy(_buffer, _item.begin, size());
    _buffer[size()] = 0;
    int64_t out;
    return (1 == sscanf(_buffer, "%" SCNd64, &out)) ? Optional<int64_t>(out) : Optional<int64_t>();
}

Optional<double> SimpleJson::getFloat() const
{
    if (Type::Float != _type)
    {
        return Optional<double>();
    }
    char _buffer[33];
    std::strncpy(_buffer, _item.begin, size());
    _buffer[size()] = 0;
    double out;
    return (1 == sscanf(_buffer, "%lf", &out)) ? Optional<double>(out) : Optional<double>();
}

double SimpleJson::getFloat(double defaultValue) const
{
    auto optionalValue = getFloat();
    return (optionalValue.isSet() ? optionalValue.get() : defaultValue);
}

bool SimpleJson::getString(const char*& out, size_t& outLen) const
{
    if (Type::String != _type || size() < 2)
    {
        return false;
    }
    out = _item.begin + 1;
    outLen = size() - 2;
    return true;
}

Optional<bool> SimpleJson::getBool() const
{
    if (Type::Boolean != _type)
    {
        return Optional<bool>();
    }
    if (size() == 4 && !std::strncmp(_item.begin, "true", 4))
    {
        return Optional<bool>(true);
    }
    if (size() == 5 && !std::strncmp(_item.begin, "false", 5))
    {
        return Optional<bool>(false);
    }
    return Optional<bool>();
}

SimpleJsonArray SimpleJson::getArray() const
{
    if (Type::Array != _type || '[' != *_item.begin)
    {
        return SimpleJsonArray(nullptr, nullptr);
    }

    auto arrayItem = Json::extractItem<'[', ']'>(_item);
    if (arrayItem.size() >= 2)
    {
        return SimpleJsonArray(arrayItem.begin + 1, arrayItem.end - 1);
    }
    else
    {
        return SimpleJsonArray(nullptr, nullptr);
    }
}

SimpleJsonArray::IterBase::IterBase(const char* start, const char* arrayEnd) : _pos(start), _arrayEnd(arrayEnd)
{
    if (_pos && _arrayEnd && _pos != _arrayEnd)
    {
        auto token = Json::nextToken(JsonToken(_pos, _arrayEnd));
        _element = SimpleJson::create(token.begin, token.end);
        if (_element.isNone())
        {
            _pos = _arrayEnd;
        }
        else
        {
            _pos = token.begin;
        }
    }
}

SimpleJsonArray::IterBase::IterBase(const SimpleJsonArray::IterBase& it) : _pos(it._pos), _arrayEnd(it._arrayEnd)
{
    if (_pos && _arrayEnd && _pos != _arrayEnd)
    {
        auto token = Json::nextToken(JsonToken(_pos, _arrayEnd));
        _element = SimpleJson::create(token.begin, token.end);
        if (_element.isNone())
        {
            _pos = _arrayEnd;
        }
        else
        {
            _pos = token.begin;
        }
    }
}

SimpleJsonArray::IterBase& SimpleJsonArray::IterBase::operator++()
{
    if (_pos == _arrayEnd)
    {
        return *this; // cannot advance a logical end iterator
    }
    _pos += _element.size();

    auto token = Json::nextToken(JsonToken(_pos, _arrayEnd));
    if (token.isComma())
    {
        token = Json::nextToken(JsonToken(token.end, _arrayEnd));
    }

    token.trim();
    _pos = token.begin;
    _element = SimpleJson::create(token.begin, token.end);
    if (_element.isNone())
    {
        _pos = _arrayEnd;
    }

    return *this;
}

size_t SimpleJsonArray::count() const
{
    size_t count = 0;
    for (auto item : *this)
    {
        ++count;
    }
    return count;
}
} // namespace utils
