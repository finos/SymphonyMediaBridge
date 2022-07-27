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
        auto wrongVal = prop.getIntValue();
        EXPECT_FALSE(wrongVal.isSet());
    }
    {
        auto prop = simpleJson.find("objName.innerEmpty");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = getStringValue(prop);
        EXPECT_EQ(val, "");
        auto wrongVal = prop.getIntValue();
        EXPECT_FALSE(wrongVal.isSet());
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
        auto val = prop.getIntValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), -3);
    }
    {
        auto prop = simpleJson.find("objName.innerDouble");
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.getFloatValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), -3.1415926);
    }
    {
        auto prop = simpleJson.find("objName.innerNull");
        EXPECT_EQ(SimpleJson::Type::Null, prop.getType());
    }
    {
        auto prop = simpleJson.find("objName.innerTrue");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        auto val = prop.getBoolValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_TRUE(val.get());
    }
    {
        auto prop = simpleJson.find("objName.innerFalse");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        auto val = prop.getBoolValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_FALSE(val.get());
    }
    {
        auto prop = simpleJson.find("objName.innerObj.bigInteger");
        EXPECT_EQ(SimpleJson::Type::Integer, prop.getType());
        auto val = prop.getIntValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), 9223372036854775807);
    }
    {
        auto prop = simpleJson.find("objName.innerObj.someDouble");
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.getFloatValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), 0.154e-10);
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
    int64_t expected[] = {1, 1, 2, 3, 5, 8, 13, 21};

    auto array = simpleJson.find("Fibonacci.numbers").getArrayValue();
    EXPECT_TRUE(array.isSet());
    EXPECT_EQ(8, array.get().size());
    int count = 0;
    for (const auto& it : array.get())
    {
        auto val = it.toJson().getIntValue();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), expected[count++]);
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

    {
        auto array = simpleJson.find("video-endpoints").getArrayValue().get();
        EXPECT_EQ(array.size(), 1);
        value = array[0].toJson().find("endpoint-id");
        EXPECT_EQ(getStringValue(value), "b469945f-856b-38c4-cf1b-0000fb452938");
        auto ssrc = array[0].toJson().find("ssrcs").getArrayValue().get();
        EXPECT_EQ(ssrc.size(), 1);
        EXPECT_EQ(ssrc[0].toJson().getIntValue().get(), 4215270161);
    }
    {
        auto array = simpleJson.find("audio-endpoints").getArrayValue().get();
        EXPECT_EQ(array.size(), 1);
        value = array[0].toJson().find("endpoint-id");
        EXPECT_EQ(getStringValue(value), "b469945f-856b-38c4-cf1b-0000fb452938");
        auto ssrc = array[0].toJson().find("ssrcs").getArrayValue().get();
        EXPECT_EQ(ssrc.size(), 1);
        EXPECT_EQ(ssrc[0].toJson().getIntValue().get(), 919268345);
    }
    {
        // Check chaining:
        auto firstVideoSsrc = simpleJson.find("video-endpoints")
                                  .getArrayValue()
                                  .get()[0]
                                  .toJson()
                                  .find("ssrcs")
                                  .getArrayValue()
                                  .get()[0]
                                  .toJson()
                                  .getIntValue()
                                  .get();
        EXPECT_EQ(firstVideoSsrc, 4215270161);
    }
}
} // namespace utils