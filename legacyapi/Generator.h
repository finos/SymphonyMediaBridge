#pragma once

#include "legacyapi/Conference.h"
#include "nlohmann/json.hpp"

namespace legacyapi
{

namespace Generator
{

nlohmann::json generate(const Conference& conference);

}

} // namespace legacyapi
