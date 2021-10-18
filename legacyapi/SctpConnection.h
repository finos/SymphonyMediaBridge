#pragma once

#include "legacyapi/Transport.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace legacyapi
{

struct SctpConnection
{
    utils::Optional<std::string> _id;
    utils::Optional<std::string> _endpoint;
    utils::Optional<std::string> _channelBundleId;
    utils::Optional<bool> _initiator;
    utils::Optional<uint32_t> _expire;
    utils::Optional<Transport> _transport;
    utils::Optional<std::string> _port;
};

} // namespace legacyapi
