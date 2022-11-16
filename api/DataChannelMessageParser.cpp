#include "api/DataChannelMessageParser.h"

namespace api
{

namespace DataChannelMessageParser
{
namespace
{
bool isLegacyApi(const utils::SimpleJson& messageJson)
{
    return messageJson.exists("colibriClass");
}
} // namespace

bool isPinnedEndpointsChanged(const utils::SimpleJson& messageJson)
{
    char messageType[200];
    if (isLegacyApi(messageJson))
    {
        messageJson["colibriClass"].getString(messageType);
        return std::strcmp("PinnedEndpointsChangedEvent", messageType) == 0;
    }
    else
    {
        auto typeItem = messageJson.find("type");
        if (typeItem.isNone())
        {
            return false;
        }
        typeItem.getString(messageType);
        return std::strcmp(messageType, "PinnedEndpointsChanged") == 0;
    }
}

utils::SimpleJson getPinnedEndpoint(const utils::SimpleJson& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return messageJson["pinnedEndpoints"];
    }
    else
    {
        return messageJson["pinnedEndpoints"];
    }
}

bool isEndpointMessage(const utils::SimpleJson& messageJson)
{
    char messageType[100];
    if (isLegacyApi(messageJson))
    {
        messageJson["colibriClass"].getString(messageType);
        return std::strcmp(messageType, "EndpointMessage") == 0;
    }
    else
    {
        messageJson["type"].getString(messageType);
        return std::strcmp(messageType, "EndpointMessage") == 0;
    }
}

utils::SimpleJson getEndpointMessageTo(const utils::SimpleJson& messageJson)
{
    return messageJson.find("to");
}

utils::SimpleJson getEndpointMessagePayload(const utils::SimpleJson& messageJson)
{
    if (isLegacyApi(messageJson))
    {
        return messageJson.find("msgPayload");
    }
    else
    {
        return messageJson.find("payload");
    }
}

bool isUserMediaMap(const utils::SimpleJson& json)
{
    char messageType[64];
    return json["type"].getString(messageType) && 0 == std::strcmp(messageType, "user-media-map");
}

bool isMinUplinkBitrate(const utils::SimpleJson& json)
{
    char messageType[64];
    return json["type"].getString(messageType) && 0 == std::strcmp(messageType, "min-uplink-bitrate");
}

uint32_t getMinUplinkBitrate(const utils::SimpleJson& json)
{
    return json["bitrateKbps"].getInt(100000);
}

} // namespace DataChannelMessageParser

} // namespace api
