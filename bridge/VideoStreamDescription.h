#pragma once
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
        if (localSsrc != 0)
        {
            v.push_back(localSsrc);
        }

        for (auto& level : simulcastSsrcs)
        {
            v.push_back(level._ssrc);
            v.push_back(level._feedbackSsrc);
        }
        return v;
    }

    std::string id;
    std::string endpointId;
    uint32_t localSsrc = 0;
    std::vector<SimulcastLevel> simulcastSsrcs;
};

} // namespace bridge
