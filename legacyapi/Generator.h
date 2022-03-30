#pragma once

#include "nlohmann/json.hpp"

namespace legacyapi
{
struct Conference;

namespace Generator
{

nlohmann::json generate(const Conference& conference);

}

} // namespace legacyapi
