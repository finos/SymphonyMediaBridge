#pragma once

#include "nlohmann/json.hpp"

namespace api
{

struct EndpointDescription;
struct ConferenceEndpoint;
struct ConferenceEndpointExtendedInfo;

namespace Generator
{

nlohmann::json generateAllocateEndpointResponse(const EndpointDescription& channelsDescription);
nlohmann::json generateConferenceEndpoint(const ConferenceEndpoint&);
nlohmann::json generateExtendedConferenceEndpoint(const ConferenceEndpointExtendedInfo&);

} // namespace Generator

} // namespace api
