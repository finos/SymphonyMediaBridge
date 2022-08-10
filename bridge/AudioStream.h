#pragma once
#include "bridge/RtpMap.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include "utils/StdExtensions.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct AudioStream
{
    AudioStream(const std::string& id,
        const std::string& endpointId,
        const uint32_t localSsrc,
        std::shared_ptr<transport::RtcTransport>& transport,
        const bool audioMixed,
        bool ssrcRewrite)
        : id(id),
          endpointId(endpointId),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSsrc(localSsrc),
          transport(transport),
          audioMixed(audioMixed),
          markedForDeletion(false),
          ssrcRewrite(ssrcRewrite),
          isConfigured(false)
    {
    }

    std::string id;
    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSsrc;
    utils::Optional<uint32_t> remoteSsrc;

    std::shared_ptr<transport::RtcTransport> transport;
    bool audioMixed;

    bridge::RtpMap rtpMap;

    bool markedForDeletion;
    bool ssrcRewrite;
    bool isConfigured;
};

} // namespace bridge
