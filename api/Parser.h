#pragma once

#include "api/AllocateConference.h"
#include "api/AllocateEndpoint.h"
#include "api/EndpointDescription.h"
#include "api/Recording.h"
#include "nlohmann/json.hpp"

namespace api
{

namespace Parser
{

AllocateConference parseAllocateConference(const nlohmann::json& data);
AllocateEndpoint parseAllocateEndpoint(const nlohmann::json& data);
EndpointDescription parsePatchEndpoint(const nlohmann::json& data, const std::string& endpointId);
Recording parseRecording(const nlohmann::json& data);

} // namespace Parser

} // namespace api
