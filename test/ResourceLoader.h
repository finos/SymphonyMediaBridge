#pragma once

#include "nlohmann/json.hpp"
#include <string>

struct ResourceLoader
{
    static std::string loadAsString(const std::string& resource);
    static nlohmann::json loadAsJson(const std::string& resource);
};