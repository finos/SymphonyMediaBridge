#pragma once
#include "bridge/AudioStream.h"
#include "bridge/VideoStream.h"
#include <cstdint>
#include <string>

namespace bridge
{

struct StreamDescription
{
    StreamDescription() = default;

    explicit StreamDescription(const AudioStream& stream) : _id(stream.id), _endpointId(stream.endpointId)
    {
        _localSsrcs.push_back(stream.localSsrc);
    }

    explicit StreamDescription(const VideoStream& stream) : _id(stream._id), _endpointId(stream._endpointId)
    {
        _localSsrcs.push_back(stream._localSsrc);
    }

    std::string _id;
    std::string _endpointId;
    std::vector<uint32_t> _localSsrcs;
};

} // namespace bridge
