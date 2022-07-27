#include "utils/SimpleJson.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace utils
{

std::string getStringValue(const SimpleJson& json)
{
    const char* str;
    size_t len;
    EXPECT_TRUE(json.getStringValue(str, len));
    return std::string(str, len);
}
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
                             "    \"someDouble\" : 0.154e-10"
                             "    },"
                             "    \"innerFalse\" : false,"
                             "}"
                             "}";
    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    {
        auto prop = simpleJson.find("name");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = getStringValue(prop);
        EXPECT_EQ(val, "some value");
        auto wrongVal = prop.valueOr((int64_t)42);
        EXPECT_EQ(wrongVal, 42);
    }
    {
        auto prop = simpleJson.find("objName.innerEmpty");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = getStringValue(prop);
        EXPECT_EQ(val, "");
        auto wrongVal = prop.valueOr((int64_t)43);
        EXPECT_EQ(wrongVal, 43);
    }
    {
        auto prop = simpleJson.find("objName");
        EXPECT_EQ(SimpleJson::Type::Object, prop.getType());
    }
    {
        auto prop = simpleJson.find("objName");
        EXPECT_EQ(SimpleJson::Type::Object, prop.getType());
        prop = prop.find("innerName");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        std::string out = getStringValue(prop);
        EXPECT_EQ(out, "innerValue");
    }
    {
        auto prop = simpleJson.find("objName.innerName");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        std::string out = getStringValue(prop);
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
    {
        auto prop = simpleJson.find("objName.innerObj.someDouble");
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.valueOr(42.0);
        EXPECT_EQ(val, 0.154e-10);
    }
}

TEST(SimpleJson, ParseArray)
{
    const std::string json = "{"
                             "\"Fibonacci\": {"
                             "\"numbers\": ["
                             "1,"
                             "1,"
                             "2,"
                             "3,"
                             "5,"
                             "8,"
                             "13,"
                             "21"
                             "]"
                             "}"
                             "}";
    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    auto propArray = simpleJson.find("Fibonacci.numbers");
    EXPECT_EQ(SimpleJson::Type::Array, propArray.getType());

    std::vector<SimpleJson> jsonArray;
    EXPECT_TRUE(propArray.getValue(jsonArray));
    EXPECT_EQ(jsonArray.size(), 8);
    int64_t expected[] = {1, 1, 2, 3, 5, 8, 13, 21};
    int count = 0;
    for (auto& it : jsonArray)
    {
        auto value = it.valueOr((int64_t)-1);
        EXPECT_EQ(value, expected[count++]);
    }
}

TEST(SimpleJson, ParseUMM)
{
    const std::string json = "{\"type\":\"user-media-map\",\"video-endpoints\":[{\"endpoint-id\":\"b469945f-856b-38c4-"
                             "cf1b-0000fb452938\",\"ssrcs\":[4215270161]}],\"audio-endpoints\":[{\"endpoint-id\":"
                             "\"b469945f-856b-38c4-cf1b-0000fb452938\",\"ssrcs\":[919268345]}]}";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    auto value = simpleJson.find("type");
    EXPECT_EQ(getStringValue(value), "user-media-map");

    std::vector<SimpleJson> endpoints, ssrc;
    {
        value = simpleJson.find("video-endpoints");
        value.getValue(endpoints);
        EXPECT_EQ(endpoints.size(), 1);
        value = endpoints[0].find("endpoint-id");
        EXPECT_EQ(getStringValue(value), "b469945f-856b-38c4-cf1b-0000fb452938");
        value = endpoints[0].find("ssrc");
        value.getValue(ssrc);
        EXPECT_EQ(ssrc.size(), 1);
        EXPECT_EQ(ssrc[0].valueOr(int64_t(42)), 4215270161);
    }

    {
        value = simpleJson.find("audio-endpoints");
        value.getValue(endpoints);
        EXPECT_EQ(endpoints.size(), 1);
        value = endpoints[0].find("endpoint-id");
        EXPECT_EQ(getStringValue(value), "b469945f-856b-38c4-cf1b-0000fb452938");
        value = endpoints[0].find("ssrc");
        value.getValue(ssrc);
        EXPECT_EQ(ssrc.size(), 1);
        EXPECT_EQ(ssrc[0].valueOr(int64_t(42)), 919268345);
    }
}
} // namespace utils