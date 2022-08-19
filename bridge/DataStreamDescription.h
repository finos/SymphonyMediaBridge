#pragma once
#include "bridge/DataStream.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace bridge
{

struct DataStreamDescription
{
    DataStreamDescription() = default;

    explicit DataStreamDescription(const DataStream& stream)
        : id(stream.id),
          endpointId(stream.endpointId),
          sctpPort(stream.localSctpPort)
    {
    }
    std::string id;
    std::string endpointId;
    utils::Optional<uint32_t> sctpPort;
};

} // namespace bridge
