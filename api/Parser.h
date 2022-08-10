#pragma once

#include "api/AllocateConference.h"
#include "api/AllocateEndpoint.h"
#include "api/ConferenceEndpoint.h"
#include "api/EndpointDescription.h"
#include "api/Recording.h"
#include "nlohmann/json.hpp"

namespace api
{

namespace Parser
{

AllocateConference parseAllocateConference(const nlohmann::json&);
AllocateEndpoint parseAllocateEndpoint(const nlohmann::json&);
EndpointDescription parsePatchEndpoint(const nlohmann::json&, const std::string& endpointId);
Recording parseRecording(const nlohmann::json&);
std::vector<ConferenceEndpoint> parseConferenceEndpoints(const nlohmann::json&);
ConferenceEndpointExtendedInfo parseEndpointExtendedInfo(const nlohmann::json&);
EndpointDescription::Ice parseIce(const nlohmann::json&);
} // namespace Parser

} // namespace api
