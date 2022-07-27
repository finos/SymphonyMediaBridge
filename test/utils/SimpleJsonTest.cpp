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
                             "    \"innerName\" : \"innerValue\""
                             "}"
                             "}";
    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    auto value = simpleJson.find("name");
    auto value2 = simpleJson.find("objName");
    auto value3 = simpleJson.find("objName.innerName");
}
} // namespace utils