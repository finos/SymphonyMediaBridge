#pragma once

#include "nlohmann/json.hpp"

inline bool exists(const nlohmann::json& json, const char* keyName)
{
    return json.find(keyName) != json.end();
}

template <typename... Args>
inline bool exists(const nlohmann::json& json, const char* key1, Args&&... keys)
{
    auto it = json.find(key1);
    if (it != json.end())
    {
        return exists(*it, std::forward<Args>(keys)...);
    }

    return false;
}
