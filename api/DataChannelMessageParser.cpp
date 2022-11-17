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
    if (isLegacyApi(messageJson))
    {
        return messageJson["colibriClass"].strcmp("PinnedEndpointsChangedEvent") == 0;
    }
    else
    {
        auto typeItem = messageJson.find("type");
        if (typeItem.isNone())
        {
            return false;
        }
        return typeItem.strcmp("PinnedEndpointsChanged") == 0;
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
    if (isLegacyApi(messageJson))
    {
        return messageJson["colibriClass"].strcmp("EndpointMessage") == 0;
    }
    else
    {
        return messageJson["type"].strcmp("EndpointMessage") == 0;
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
    return json["type"].strcmp("user-media-map") == 0;
}

bool isMinUplinkBitrate(const utils::SimpleJson& json)
{
    return json["type"].strcmp("min-uplink-bitrate") == 0;
}

uint32_t getMinUplinkBitrate(const utils::SimpleJson& json)
{
    return json["bitrateKbps"].getInt(100000);
}

} // namespace DataChannelMessageParser

} // namespace api
