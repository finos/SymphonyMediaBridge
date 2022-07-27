#include "utils/SimpleJson.h"
#include <gtest/gtest.h>
#include <string>

namespace utils
{
TEST(SimpleJson, ParseValidJson)
{
    const std::string json = "{"
                             "\"name\" : \"value\","
                             "\"objName\": {"
                             "    \"innerName\" : \"innerValue\","
                             "    \"innerInteger\" : -3,"
                             "    \"innerDouble\" : -3.1415926,"
                             "    \"innerNull\" : null,"
                             "    \"innerTrue\" : true,"
                             "    \"innerObj\" : {  "
                             "    \"bigInteger\" : 18446744073709551615,"
                             "    },"
                             "    \"innerFalse\" : false,"
                             "}"
                             "}";
    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    auto value = simpleJson.find("name");
    EXPECT_EQ(SimpleJson::Type::String, value.getType());

    value = simpleJson.find("objName");
    EXPECT_EQ(SimpleJson::Type::Object, value.getType());

    value = simpleJson.find("objName.innerName");
    EXPECT_EQ(SimpleJson::Type::String, value.getType());

    value = simpleJson.find("objName.innerInteger");
    EXPECT_EQ(SimpleJson::Type::Integer, value.getType());

    value = simpleJson.find("objName.innerDouble");
    EXPECT_EQ(SimpleJson::Type::Float, value.getType());

    value = simpleJson.find("objName.innerNull");
    EXPECT_EQ(SimpleJson::Type::Null, value.getType());

    value = simpleJson.find("objName.innerTrue");
    EXPECT_EQ(SimpleJson::Type::Boolean, value.getType());

    value = simpleJson.find("objName.innerFalse");
    EXPECT_EQ(SimpleJson::Type::Boolean, value.getType());

    value = simpleJson.find("objName.innerObj.bigInteger");
    EXPECT_EQ(SimpleJson::Type::Integer, value.getType());
}
} // namespace utils