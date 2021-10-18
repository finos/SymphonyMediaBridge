#include "legacyapi/DataChannelMessageParser.h"

namespace legacyapi
{

namespace DataChannelMessageParser
{

bool isPinnedEndpointsChanged(const nlohmann::json& messageJson)
{
    return messageJson["colibriClass"].get<std::string>().compare("PinnedEndpointsChangedEvent") == 0;
}

const nlohmann::json& getPinnedEndpoint(const nlohmann::json& messageJson)
{
    return messageJson["pinnedEndpoints"];
}

bool isEndpointMessage(const nlohmann::json& messageJson)
{
    return messageJson["colibriClass"].get<std::string>().compare("EndpointMessage") == 0;
}

nlohmann::json::const_iterator getEndpointMessageTo(const nlohmann::json& messageJson)
{
    return messageJson.find("to");
}

nlohmann::json::const_iterator getEndpointMessagePayload(const nlohmann::json& messageJson)
{
    return messageJson.find("msgPayload");
}

} // namespace DataChannelMessageParser

} // namespace legacyapi
