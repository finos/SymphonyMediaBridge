#include "api/DataChannelMessageParser.h"

#if ENABLE_LEGACY_API
#include "legacyapi/DataChannelMessageParser.h"
#endif

namespace api
{

namespace DataChannelMessageParser
{

bool isPinnedEndpointsChanged(const nlohmann::json& messageJson)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessageParser::isPinnedEndpointsChanged(messageJson);
#else
    return messageJson["type"].get<std::string>().compare("PinnedEndpointsChanged") == 0;
#endif
}

const nlohmann::json& getPinnedEndpoint(const nlohmann::json& messageJson)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessageParser::getPinnedEndpoint(messageJson);
#else
    return messageJson["pinnedEndpoints"];
#endif
}

bool isEndpointMessage(const nlohmann::json& messageJson)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessageParser::isEndpointMessage(messageJson);
#else
    return messageJson["type"].get<std::string>().compare("EndpointMessage") == 0;
#endif
}

nlohmann::json::const_iterator getEndpointMessageTo(const nlohmann::json& messageJson)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessageParser::getEndpointMessageTo(messageJson);
#else
    return messageJson.find("to");
#endif
}

nlohmann::json::const_iterator getEndpointMessagePayload(const nlohmann::json& messageJson)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessageParser::getEndpointMessagePayload(messageJson);
#else
    return messageJson.find("payload");
#endif
}

} // namespace DataChannelMessageParser

} // namespace api
