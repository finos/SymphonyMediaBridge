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
    DataStream(const std::string& id_,
        const std::string& endpointId_,
        std::shared_ptr<transport::RtcTransport>& transport_,
        utils::Optional<uint32_t> idleTimeout)
        : id(id_),
          endpointId(endpointId_),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSctpPort(rand() % 19000 + 1000),
          transport(transport_),
          markedForDeletion(false),
          isConfigured(false),
          idleTimeoutSeconds(idleTimeout.valueOr(0))
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
    const uint32_t idleTimeoutSeconds;
};

} // namespace bridge
