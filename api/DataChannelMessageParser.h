#pragma once

#include "nlohmann/json.hpp"
#include "utils/SimpleJson.h"

namespace api
{

namespace DataChannelMessageParser
{

bool isEndpointMessage(const utils::SimpleJson& messageJson);
utils::SimpleJson getEndpointMessageTo(const utils::SimpleJson& messageJson);
utils::SimpleJson getEndpointMessagePayload(const utils::SimpleJson& messageJson);

bool isPinnedEndpointsChanged(const utils::SimpleJson& messageJson);
utils::SimpleJson getPinnedEndpoint(const utils::SimpleJson& messageJson);

bool isUserMediaMap(const utils::SimpleJson&);

bool isMinUplinkBitrate(const utils::SimpleJson&);
uint32_t getMinUplinkBitrate(const utils::SimpleJson&);

} // namespace DataChannelMessageParser

} // namespace api
