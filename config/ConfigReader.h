#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace config
{

/*
Helps declare your config and uses the declarations
to read values from file or string.

Example usage:

class ApplicationConfig: public ConfigReader
{
public:
    CFG_PROP(std::string, test, "defvalue", true);
};

void main()
{
    ApplicationConfig cfg;
    cfg.readFromFile("/tmp/testconfig.json");

    printf("test is set to %s", cfg.test.c_str());
}
*/
class ConfigReader
{
public:
    bool readFromFile(const std::string& fileName);
    bool readFromString(const std::string& json);

protected:
    struct IProperty
    {
    protected:
        friend class ConfigReader;
        virtual bool read(nlohmann::json& objectNode) = 0;
        virtual const std::string& getName() const = 0;
    };

    bool parse(const char* buffer);

    template <typename T>
    class PropertyImpl : public ConfigReader::IProperty
    {
    public:
        PropertyImpl(const char* location,
            T defaultValue,
            std::vector<IProperty*>& properties,
            const std::string& groupName)
            : _name(makeName(location, groupName)),
              _value(defaultValue),
              _optional(true)
        {
            properties.push_back(this);
        }

        PropertyImpl(const char* location, std::vector<IProperty*>& properties, const std::string& groupName)
            : _name(makeName(location, groupName)),
              _value(T()),
              _optional(false)
        {
            properties.push_back(this);
        }

        const T& get() const { return _value; }
        const T& operator=(const T& v)
        {
            _value = v;
            return _value;
        }

        operator const T&() const { return _value; }

    protected:
        bool read(nlohmann::json& objectNode) override
        {
            auto& n = objectNode[_name];
            if (!_optional && n.is_null())
            {
                _value = T();
                return false;
            }
            if (!n.is_null())
            {
                _value = n.template get<T>();
            }
            return true;
        }

        const std::string& getName() const override { return _name; }

    private:
        const std::string _name;
        T _value;
        const bool _optional;

        static std::string makeName(const char* location, const std::string& groupName)
        {
            return groupName.empty() ? location : groupName + "." + location;
        }
    };

    std::vector<IProperty*> _properties;
    std::string _groupName;
};
} // namespace config

#define __CFG_READER_MERGE_HELPER(x, y) x##y
#define __CFG_READER_MERGE_HELPER2(x, y) __CFG_READER_MERGE_HELPER(x, y)

#define CFG_GROUP()                                                                                                    \
    struct __CFG_READER_MERGE_HELPER2(Group, __LINE__)                                                                 \
    {                                                                                                                  \
        const std::string _groupName;                                                                                  \
        std::vector<IProperty*>& _properties;                                                                          \
        __CFG_READER_MERGE_HELPER2(Group, __LINE__)                                                                    \
        (const std::string& parent, const std::string& name, std::vector<IProperty*>& p)                               \
            : _groupName(parent.empty() ? name : parent + "." + name),                                                 \
              _properties(p)                                                                                           \
        {                                                                                                              \
        }
#define CFG_GROUP_END(name)                                                                                            \
    }                                                                                                                  \
    name{_groupName, #name, _properties};

#define CFG_PROP(type, name, defaultValue) PropertyImpl<type> name = {#name, defaultValue, _properties, _groupName}

#define CFG_MANDATORY_PROP(type, name) PropertyImpl<type> name = {#name, _properties, _groupName}
