#include "utils/SimpleJson.h"
#include <gtest/gtest.h>
#include <string>

namespace utils
{
TEST(SimpleJson, ParseValidJson)
{
    const std::string json = "{"
                             "\"name\" : \"some value\","
                             "\"objName\": {"
                             "    \"innerName\" : \"innerValue\","
                             "    \"innerEmpty\" : \"\","
                             "    \"innerInteger\" : -3,"
                             "    \"innerDouble\" : -3.1415926,"
                             "    \"innerNull\" : null,"
                             "    \"innerTrue\" : true,"
                             "    \"innerObj\" : {  "
                             "    \"bigInteger\" : 9223372036854775807,"
                             "    },"
                             "    \"innerFalse\" : false,"
                             "}"
                             "}";
    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    {
        auto prop = simpleJson.find("name");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = prop.valueOr(std::string("default"));
        EXPECT_EQ(val, "some value");
        auto wrongVal = prop.valueOr((int64_t)42);
        EXPECT_EQ(wrongVal, 42);
    }
    {
        auto prop = simpleJson.find("objName.innerEmpty");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = prop.valueOr(std::string("default"));
        EXPECT_EQ(val, "");
        auto wrongVal = prop.valueOr((int64_t)43);
        EXPECT_EQ(wrongVal, 43);
    }
    {
        auto prop = simpleJson.find("objName");
        EXPECT_EQ(SimpleJson::Type::Object, prop.getType());
    }

    {
        auto prop = simpleJson.find("objName.innerName");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        std::string out = "value to overwrite";
        EXPECT_TRUE(prop.getValue(out));
        EXPECT_EQ(out, "innerValue");
    }

    {
        auto prop = simpleJson.find("objName.innerInteger");
        EXPECT_EQ(SimpleJson::Type::Integer, prop.getType());
        auto val = prop.valueOr((int64_t)42);
        EXPECT_EQ(val, -3);
    }

    {
        auto prop = simpleJson.find("objName.innerDouble");
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.valueOr(42.0);
        EXPECT_EQ(val, -3.1415926);
    }

    {
        auto prop = simpleJson.find("objName.innerNull");
        EXPECT_EQ(SimpleJson::Type::Null, prop.getType());
    }

    {
        auto prop = simpleJson.find("objName.innerTrue");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        bool out;
        EXPECT_TRUE(prop.getValue(out));
        EXPECT_TRUE(out);
    }

    {
        auto prop = simpleJson.find("objName.innerFalse");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        bool out;
        EXPECT_TRUE(prop.getValue(out));
        EXPECT_FALSE(out);
    }

    {
        auto prop = simpleJson.find("objName.innerObj.bigInteger");
        EXPECT_EQ(SimpleJson::Type::Integer, prop.getType());
        auto val = prop.valueOr((int64_t)42);
        EXPECT_EQ(val, 9223372036854775807);
    }
}
} // namespace utils