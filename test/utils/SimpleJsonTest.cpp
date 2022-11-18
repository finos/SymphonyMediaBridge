#include "utils/SimpleJson.h"
#include "logger/Logger.h"
#include "memory/Array.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace utils
{

TEST(SimpleJson, ParseValidJson)
{
    const std::string json = R"(
        {
            "name" : "some value",
            "objName": {
                "innerName" : "innerValue",
                "innerEmpty" : "",
                "innerInteger" : -3,
                "innerDouble" : -3.1415926,
                "innerNull" : null,
                "innerTrue": true,
                "innerObj": {"bigInteger" : 9223372036854775807, "someDouble" : 0.154e-10},
                "innerFalse" : false,
            }
        }
    )";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    {
        auto prop = simpleJson.find("name");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = prop.getString();
        EXPECT_EQ(val, "some value");
        auto wrongVal = prop.getInt();
        EXPECT_FALSE(wrongVal.isSet());
    }
    {
        auto prop = simpleJson.find("objName.innerEmpty");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        auto val = prop.getString();
        EXPECT_EQ(val, "");
        auto wrongVal = prop.getInt();
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
        std::string out = prop.getString();
        EXPECT_EQ(out, "innerValue");
    }
    {
        auto prop = simpleJson.find("objName.innerName");
        EXPECT_EQ(SimpleJson::Type::String, prop.getType());
        std::string out = prop.getString();
        EXPECT_EQ(out, "innerValue");
    }
    {
        auto prop = simpleJson.find("objName.innerInteger");
        EXPECT_EQ(SimpleJson::Type::Integer, prop.getType());
        auto val = prop.getInt();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), -3);
    }
    {
        auto prop = simpleJson["objName.innerDouble"];
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.getFloat();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), -3.1415926);
    }
    {
        auto prop = simpleJson["objName"]["innerNull"];
        EXPECT_EQ(SimpleJson::Type::Null, prop.getType());
    }
    {
        auto prop = simpleJson.find("objName.innerTrue");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        auto val = prop.getBool();
        EXPECT_TRUE(val.isSet());
        EXPECT_TRUE(val.get());
    }
    {
        auto prop = simpleJson.find("objName.innerFalse");
        EXPECT_EQ(SimpleJson::Type::Boolean, prop.getType());
        auto val = prop.getBool();
        EXPECT_TRUE(val.isSet());
        EXPECT_FALSE(val.get());
    }
    {
        auto prop = simpleJson.find("objName.innerObj.bigInteger");
        EXPECT_EQ(SimpleJson::Type::Integer, prop.getType());
        auto val = prop.getInt();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), 9223372036854775807);
    }
    {
        auto prop = simpleJson.find("objName.innerObj.someDouble");
        EXPECT_EQ(SimpleJson::Type::Float, prop.getType());
        auto val = prop.getFloat();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), 0.154e-10);
    }
}

TEST(SimpleJson, ParseArray)
{
    const std::string json = R"({
            "Fibonacci": { "numbers": [1,1, 2,3,5,8, 13, 21]}}
    )";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    int64_t expected[] = {1, 1, 2, 3, 5, 8, 13, 21};

    auto array = simpleJson.find("Fibonacci.numbers").getArray();
    EXPECT_FALSE(array.empty());

    int count = 0;
    for (const auto& item : array)
    {
        auto val = item.getInt();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), expected[count++]);
    }
}

TEST(SimpleJson, ParseMalformedArray)
{
    const std::string json = R"({
            "Fibonacci": { "numbers": [1,1, 2,3,5,8, 13, 21,]}}
    )";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    int64_t expected[] = {1, 1, 2, 3, 5, 8, 13, 21};

    auto array = simpleJson.find("Fibonacci.numbers").getArray();
    EXPECT_FALSE(array.empty());

    int count = 0;
    for (const auto& item : array)
    {
        auto val = item.getInt();
        EXPECT_TRUE(val.isSet());
        EXPECT_EQ(val.get(), expected[count++]);
    }
}

TEST(SimpleJson, ParseUMM)
{
    const std::string json = R"(
        {
        "type":"user-media-map",
        "video-endpoints":[{"endpoint-id":"b469945f-856b-38c4-cf1b-0000fb452938","ssrcs":[4215270161]}],
        "audio-endpoints":[{"endpoint-id":"b469945f-856b-38c4-cf1b-0000fb452938","ssrcs":[919268345]}]
        }
        )";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());
    auto value = simpleJson.find("type");
    EXPECT_EQ(value.getString(), "user-media-map");

    {
        auto array = simpleJson.find("video-endpoints").getArray();

        value = array.front().find("endpoint-id");
        EXPECT_EQ(value.getString(), "b469945f-856b-38c4-cf1b-0000fb452938");
        auto ssrc = array.front().find("ssrcs").getArray();

        EXPECT_EQ(ssrc.front().getInt().get(), 4215270161);
    }
    {
        auto array = simpleJson.find("audio-endpoints").getArray();
        EXPECT_EQ(array.count(), 1);
        value = array.front().find("endpoint-id");
        EXPECT_EQ(value.getString(), "b469945f-856b-38c4-cf1b-0000fb452938");
        auto ssrcs = array.front().find("ssrcs").getArray();
        EXPECT_EQ(ssrcs.count(), 1);
        EXPECT_EQ(ssrcs.front().getInt().get(), 919268345);
    }
    {
        // Check chaining:
        auto firstVideoSsrc =
            simpleJson.find("video-endpoints").getArray().front().find("ssrcs").getArray().front().getInt().get();
        EXPECT_EQ(firstVideoSsrc, 4215270161);
    }
}

TEST(SimpleJson, ParseLargeUMM)
{
    const std::string json = R"(
        {
        "type":"user-media-map",
        "video-endpoints":[{"endpoint-id":"b469945f-856b-38c4-cf1b-0000fb452938","ssrcs":[4215270161]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-000023984bc7",  "ssrcs":[1115270161]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-0000fb452238", "ssrcs":[4123210161]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-00adc5752938","ssrcs":[4983473161, 1238923  ]}],
        "audio-endpoints":[{"endpoint-id":"b469945f-856b-38c4-cf1b-0000fb452938","ssrcs":[919268345]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-000192939938","ssrcs":[139268345]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-000aed982938","ssrcs":[523768345]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-00084bafec38","ssrcs":[112354845]},
            {"endpoint-id":"b469945f-856b-38c4-cf1b-0007322b8938","ssrcs":[833583485]}]
        }
        )";

    auto simpleJson = SimpleJson::create(json.c_str(), json.length());

    auto videoEndpoints = simpleJson["video-endpoints"].getArray();
    auto audioEndpoints = simpleJson["audio-endpoints"].getArray();

    memory::Array<uint32_t, 10> videoSsrc;
    memory::Array<uint32_t, 10> audioSsrc;
    for (auto endpointMap : videoEndpoints)
    {
        char guuid[64];
        EXPECT_TRUE(endpointMap.exists("endpoint-id"));
        if (endpointMap["endpoint-id"].getString(guuid))
        {
            logger::debug("video %s -> %u",
                "UmmTest",
                guuid,
                endpointMap["ssrcs"].getArray().front().getInt<uint32_t>(0));
        }

        for (auto ssrc : endpointMap["ssrcs"].getArray())
        {
            videoSsrc.push_back(ssrc.getInt<uint32_t>(0));
        }
    }
    for (auto endpointMap : audioEndpoints)
    {
        for (auto ssrc : endpointMap["ssrcs"].getArray())
        {
            audioSsrc.push_back(ssrc.getInt<uint32_t>(0));
        }
    }

    EXPECT_EQ(videoEndpoints.count(), 4);
    EXPECT_EQ(audioEndpoints.count(), 5);

    EXPECT_EQ(audioSsrc.size(), 5);
    EXPECT_EQ(videoSsrc.size(), 5);
}
} // namespace utils
