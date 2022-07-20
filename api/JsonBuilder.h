#pragma once
#include <cstring>

namespace JsonBuilder
{

template <typename TBuilder>
class JsonObject
{
    TBuilder& _builder;

public:
    JsonObject(TBuilder& builder, const char* name) : _builder(builder)
    {
        assert(name);
        if (!(_builder.empty() || _builder.endsWidth('[') || _builder.endsWidth('{')))
        {
            _builder.append(",");
        }

        _builder.append("\"");
        _builder.append(name);
        _builder.append("\":");

        _builder.append("{");
    };

    JsonObject(TBuilder& builder) : _builder(builder)
    {
        if (!(_builder.empty() || _builder.endsWidth('[') || _builder.endsWidth('{')))
        {
            _builder.append(",");
        }

        _builder.append("{");
    };

    ~JsonObject() { _builder.append("}"); }
};

template <typename TBuilder>
class JsonArray
{
    TBuilder& _builder;

public:
    JsonArray(TBuilder& builder, const char* name) : _builder(builder)
    {
        if (!_builder.endsWidth('{'))
        {
            _builder.append(",");
        }
        _builder.append("\"");
        _builder.append(name);
        _builder.append("\":[");
    };

    ~JsonArray() { _builder.append("]"); }
};

template <typename TBuilder>
void addProperty(TBuilder& builder, const char* name, const char* value)
{
    if (!builder.endsWidth('{'))
    {
        builder.append(",");
    }
    builder.append("\"");
    builder.append(name);
    builder.append("\":\"");
    builder.append(value);
    builder.append("\"");
}

template <typename TBuilder>
void addProperty(TBuilder& builder, const char* name, const std::string& value)
{
    addProperty(builder, name, value.c_str());
}

template <typename TBuilder, typename T>
void addProperty(TBuilder& builder, const char* name, const T& value)
{
    if (!builder.endsWidth('{'))
    {
        builder.append(",");
    }
    builder.append("\"");
    builder.append(name);
    builder.append("\":");
    builder.append(value);
}

template <typename TBuilder>
void addArrayValue(TBuilder& builder, const char* value)
{
    if (!builder.endsWidth('['))
    {
        builder.append(",");
    }
    builder.append("\"");
    builder.append(value);
    builder.append("\"");
}

template <typename TBuilder>
void addArrayValue(TBuilder& builder, const std::string& value)
{
    addArrayValue(builder, value.c_str());
}

template <typename TBuilder, typename T>
void addArrayValue(TBuilder& builder, const T& value)
{
    if (!builder.endsWidth('['))
    {
        builder.append(",");
    }
    builder.append(value);
}

}; // namespace JsonBuilder
