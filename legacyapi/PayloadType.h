#pragma once

#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace legacyapi
{

struct PayloadType
{
    struct RtcpFb
    {
        std::string _type;
        utils::Optional<std::string> _subtype;
    };
    uint32_t _id;
    std::string _name;
    std::string _clockRate;
    utils::Optional<std::string> _channels;
    std::vector<std::pair<std::string, std::string>> _parameters;
    std::vector<RtcpFb> _rtcpFbs;
};

} // namespace legacyapi
