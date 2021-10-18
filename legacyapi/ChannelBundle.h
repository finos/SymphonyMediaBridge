#pragma once

#include "legacyapi/Transport.h"
#include <string>

namespace legacyapi
{

struct ChannelBundle
{
    std::string _id;
    Transport _transport;
};

} // namespace legacyapi
