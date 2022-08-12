#pragma once
#include "bridge/AudioStream.h"
#include <cstdint>
#include <string>

namespace bridge
{

struct AudioStreamDescription
{
    AudioStreamDescription() = default;

    explicit AudioStreamDescription(const AudioStream& stream) : id(stream.id), endpointId(stream.endpointId)
    {
        ssrcs.push_back(stream.localSsrc);
    }

    std::string id;
    std::string endpointId;
    std::vector<uint32_t> ssrcs;
};

} // namespace bridge
