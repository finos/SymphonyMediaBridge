#pragma once

#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace legacyapi
{

struct Candidate
{
    uint32_t _generation;
    uint32_t _component;
    std::string _protocol;
    uint32_t _port;
    std::string _ip;
    utils::Optional<uint32_t> _relPort;
    utils::Optional<std::string> _relAddr;
    std::string _foundation;
    uint32_t _priority;
    std::string _type;
    uint32_t _network;
};

} // namespace legacyapi
