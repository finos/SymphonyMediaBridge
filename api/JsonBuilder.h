#pragma once
#include <cstring>

/**
 * Helper objects used to format a json body. Use code blocks to manage life time of JsonArray and JsonObject to set the
 * braces inside the json. You have to add elements in order. You cannot add ddproperties to an object higher in
 * hierarchy until you have closed the current object.
 * example:
 * Stringbuilder b;
 * {
 *  JsonObject json(b);
 *  {
 *      JsonObject foo(b, "foo");
 *      foo.addProperty("test", 56);
 *      foo.addProperty("tar", "string");
 *      // json.addProperty("error",7); this is invalid
 *      JsonArray bar("bar");
 *      for (int i = 0; i < 25;++i)
 *      {
 *          bar.addElement(i*7);
 *      }
 *  }
 * }
 * */
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

    void addProperty(const char* name, const char* value)
    {
        if (!_builder.endsWidth('{'))
        {
            _builder.append(",");
        }
        _builder.append("\"");
        _builder.append(name);
        _builder.append("\":\"");
        _builder.append(value);
        _builder.append("\"");
    }

    void addProperty(const char* name, const std::string& value) { addProperty(name, value.c_str()); }

    template <typename T>
    void addProperty(const char* name, const T& value)
    {
        if (!_builder.endsWidth('{'))
        {
            _builder.append(",");
        }
        _builder.append("\"");
        _builder.append(name);
        _builder.append("\":");
        _builder.append(value);
    }
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

    void addElement(const char* value)
    {
        if (!_builder.endsWidth('['))
        {
            _builder.append(",");
        }
        _builder.append("\"");
        _builder.append(value);
        _builder.append("\"");
    }

    void addElement(const std::string& value) { addElement(value.c_str()); }

    template <typename T>
    void addElement(const T& value)
    {
        if (!_builder.endsWidth('['))
        {
            _builder.append(",");
        }
        _builder.append(value);
    }
};

}; // namespace JsonBuilder
