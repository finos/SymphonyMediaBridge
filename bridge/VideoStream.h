#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct VideoStream
{
    VideoStream(const std::string& id,
        const std::string& endpointId,
        const uint32_t localSsrc,
        std::shared_ptr<transport::RtcTransport>& transport,
        bool ssrcRewrite)
        : id(id),
          endpointId(endpointId),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSsrc(localSsrc),
          simulcastStream{0},
          transport(transport),
          markedForDeletion(false),
          ssrcRewrite(ssrcRewrite),
          isConfigured(false)
    {
    }

    std::string id;
    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSsrc;
    SimulcastStream simulcastStream;
    utils::Optional<SimulcastStream> secondarySimulcastStream;
    std::shared_ptr<transport::RtcTransport> transport;

    bridge::RtpMap rtpMap;
    bridge::RtpMap feedbackRtpMap;

    SsrcWhitelist ssrcWhitelist;

    bool markedForDeletion;
    bool ssrcRewrite;
    bool isConfigured;
};

} // namespace bridge
