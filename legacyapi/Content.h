#pragma once

#include "legacyapi/Channel.h"
#include "legacyapi/SctpConnection.h"
#include <string>
#include <vector>

namespace legacyapi
{

struct Content
{
    std::string _name;
    std::vector<Channel> _channels;
    std::vector<SctpConnection> _sctpConnections;
};

} // namespace legacyapi
