#pragma once
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include "utils/StdExtensions.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct DataStream
{
    DataStream(const std::string& id,
        const std::string& endpointId,
        std::shared_ptr<transport::RtcTransport>& transport)
        : _id(id),
          _endpointId(endpointId),
          _endpointIdHash(utils::hash<std::string>{}(_endpointId)),
          _localSctpPort(rand() % 19000 + 1000),
          _transport(transport),
          _markedForDeletion(false),
          _isConfigured(false)
    {
    }

    std::string _id;
    std::string _endpointId;
    size_t _endpointIdHash;
    uint32_t _localSctpPort;
    utils::Optional<uint32_t> _remoteSctpPort;

    std::shared_ptr<transport::RtcTransport> _transport;

    bool _markedForDeletion;
    bool _isConfigured;
};

} // namespace bridge
