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
        : id(id),
          endpointId(endpointId),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSctpPort(rand() % 19000 + 1000),
          transport(transport),
          markedForDeletion(false),
          isConfigured(false)
    {
    }

    std::string id;
    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSctpPort;
    utils::Optional<uint32_t> remoteSctpPort;

    std::shared_ptr<transport::RtcTransport> transport;

    bool markedForDeletion;
    bool isConfigured;
};

} // namespace bridge
