#pragma once

#include "nlohmann/json.hpp"

namespace api
{

struct EndpointDescription;
struct ConferenceEndpoint;
struct ConferenceEndpointExtendedInfo;
struct BarbellDescription;

namespace Generator
{

nlohmann::json generateAllocateEndpointResponse(const EndpointDescription& channelsDescription);
nlohmann::json generateConferenceEndpoint(const ConferenceEndpoint&);
nlohmann::json generateExtendedConferenceEndpoint(const ConferenceEndpointExtendedInfo&);
nlohmann::json generateAllocateBarbellResponse(const BarbellDescription& channelsDescription);

} // namespace Generator

} // namespace api
