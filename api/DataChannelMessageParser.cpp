#include "api/DataChannelMessageParser.h"
#include "legacyapi/DataChannelMessageParser.h"

namespace api
{

namespace DataChannelMessageParser
{
namespace
{
bool isLegacyApi(const nlohmann::json& messageJson)
{
    return messageJson.find("colibriClass") != messageJson.end();
}
} // namespace

bool isPinnedEndpointsChanged(const nlohmann::json& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return legacyapi::DataChannelMessageParser::isPinnedEndpointsChanged(messageJson);
    }
    else
    {
        return messageJson["type"].get<std::string>().compare("PinnedEndpointsChanged") == 0;
    }
}

const nlohmann::json& getPinnedEndpoint(const nlohmann::json& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return legacyapi::DataChannelMessageParser::getPinnedEndpoint(messageJson);
    }
    else
    {
        return messageJson["pinnedEndpoints"];
    }
}

bool isEndpointMessage(const nlohmann::json& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return legacyapi::DataChannelMessageParser::isEndpointMessage(messageJson);
    }
    else
    {
        return messageJson["type"].get<std::string>().compare("EndpointMessage") == 0;
    }
}

nlohmann::json::const_iterator getEndpointMessageTo(const nlohmann::json& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return legacyapi::DataChannelMessageParser::getEndpointMessageTo(messageJson);
    }
    else
    {
        return messageJson.find("to");
    }
}

nlohmann::json::const_iterator getEndpointMessagePayload(const nlohmann::json& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return legacyapi::DataChannelMessageParser::getEndpointMessagePayload(messageJson);
    }
    else
    {
        return messageJson.find("payload");
    }
}

} // namespace DataChannelMessageParser

} // namespace api
