#pragma once
#include <string>

/**
 * Helper objects used to format a json body. Use code blocks to manage life time of Array and Object to set the
 * braces inside the json. You have to add elements in order. You cannot add properties to an object higher in
 * hierarchy until you have closed the current object.
 * example:
 * Stringbuilder b;
 * {
 *  Object json(b);
 *  {
 *      Object foo(b, "foo");
 *      foo.addProperty("test", 56);
 *      foo.addProperty("tar", "string");
 *      // json.addProperty("error",7); this is invalid
 *      Array bar("bar");
 *      for (int i = 0; i < 25;++i)
 *      {
 *          bar.addElement(i*7);
 *      }
 *  }
 * }
 * */
namespace json
{
namespace writer
{

template <typename TBuilder>
class Object
{
    TBuilder& _builder;

public:
    Object(TBuilder& builder, const char* name) : _builder(builder)
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

    Object(TBuilder& builder) : _builder(builder)
    {
        if (!(_builder.empty() || _builder.endsWidth('[') || _builder.endsWidth('{')))
        {
            _builder.append(",");
        }

        _builder.append("{");
    };

    ~Object() { _builder.append("}"); }

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
class Array
{
    TBuilder& _builder;

public:
    Array(TBuilder& builder, const char* name) : _builder(builder)
    {
        if (!_builder.endsWidth('{'))
        {
            _builder.append(",");
        }
        _builder.append("\"");
        _builder.append(name);
        _builder.append("\":[");
    };

    ~Array() { _builder.append("]"); }

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

template <typename TStringBuilder>
Object<TStringBuilder> createObjectWriter(TStringBuilder& builder)
{
    return Object<TStringBuilder>(builder);
}

template <typename TStringBuilder>
Object<TStringBuilder> createObjectWriter(TStringBuilder& builder, const char* propertyName)
{
    return Object<TStringBuilder>(builder, propertyName);
}

template <typename TStringBuilder>
Array<TStringBuilder> createArrayWriter(TStringBuilder& builder, const char* propertyName)
{
    return Array<TStringBuilder>(builder, propertyName);
}

} // namespace writer
} // namespace json
