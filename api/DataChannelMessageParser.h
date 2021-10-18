#pragma once

#include "nlohmann/json.hpp"

namespace api
{

namespace DataChannelMessageParser
{

bool isPinnedEndpointsChanged(const nlohmann::json& messageJson);
const nlohmann::json& getPinnedEndpoint(const nlohmann::json& messageJson);

bool isEndpointMessage(const nlohmann::json& messageJson);
nlohmann::json::const_iterator getEndpointMessageTo(const nlohmann::json& messageJson);
nlohmann::json::const_iterator getEndpointMessagePayload(const nlohmann::json& messageJson);

} // namespace DataChannelMessageParser

} // namespace api
