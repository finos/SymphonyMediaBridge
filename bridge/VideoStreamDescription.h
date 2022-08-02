#pragma once
#include "api/SimulcastGroup.h"
#include "bridge/VideoStream.h"
#include <cstdint>
#include <string>

namespace bridge
{
struct VideoStreamDescription
{
    VideoStreamDescription() = default;
    explicit VideoStreamDescription(const VideoStream& stream) : id(stream._id), endpointId(stream._endpointId)
    {
        localSsrc = stream._localSsrc;
    }

    std::vector<uint32_t> getSsrcs()
    {
        std::vector<uint32_t> v;
        v.reserve(sources.size() + 1);
        if (localSsrc != 0)
        {
            v.push_back(localSsrc);
        }

        for (auto& level : sources)
        {
            v.push_back(level.main);
            v.push_back(level.feedback);
        }
        return v;
    }

    std::string id;
    std::string endpointId;
    uint32_t localSsrc = 0;
    std::vector<api::SsrcPair> sources;
};

} // namespace bridge
