#pragma once

#include "legacyapi/Conference.h"
#include "nlohmann/json.hpp"

namespace legacyapi
{

namespace Parser
{

Conference parse(const nlohmann::json& data);

}

} // namespace legacyapi
