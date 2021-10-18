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
        : _id(stream._id),
          _endpointId(stream._endpointId),
          _sctpPort(stream._localSctpPort)
    {
    }
    std::string _id;
    std::string _endpointId;
    utils::Optional<uint32_t> _sctpPort;
};

} // namespace bridge
