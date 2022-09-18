#pragma once

#include "nlohmann/json.hpp"
#include "utils/SimpleJson.h"

namespace api
{

namespace DataChannelMessageParser
{

bool isPinnedEndpointsChanged(const nlohmann::json& messageJson);
const nlohmann::json& getPinnedEndpoint(const nlohmann::json& messageJson);

bool isEndpointMessage(const nlohmann::json& messageJson);
nlohmann::json::const_iterator getEndpointMessageTo(const nlohmann::json& messageJson);
nlohmann::json::const_iterator getEndpointMessagePayload(const nlohmann::json& messageJson);

bool isUserMediaMap(const utils::SimpleJson&);

bool isMinUplinkBitrate(const utils::SimpleJson&);
uint32_t getMinUplinkBitrate(const utils::SimpleJson&);

} // namespace DataChannelMessageParser

} // namespace api
