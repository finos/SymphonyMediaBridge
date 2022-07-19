#pragma once

#include "nlohmann/json.hpp"

namespace api
{

struct EndpointDescription;
struct ConferenceEndpoint;

namespace Generator
{

nlohmann::json generateAllocateEndpointResponse(const EndpointDescription& channelsDescription);
nlohmann::json generateConferenceEndpoint(const ConferenceEndpoint&);

} // namespace Generator

} // namespace api
