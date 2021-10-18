#pragma once

#include "nlohmann/json.hpp"

namespace api
{

struct EndpointDescription;

namespace Generator
{

nlohmann::json generateAllocateEndpointResponse(const EndpointDescription& channelsDescription);


}

} // namespace api
