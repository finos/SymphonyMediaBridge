#pragma once

#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace legacyapi
{

struct Connection
{
    uint32_t _port;
    std::string _ip;
};

} // namespace legacyapi
