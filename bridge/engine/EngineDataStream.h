#pragma once

#include "transport/RtcTransport.h"
#include "webrtc/WebRtcDataStream.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <unistd.h>

namespace bridge
{

struct EngineDataStream
{
    EngineDataStream(const std::string& endpointId,
        const size_t endpointIdHash,
        transport::RtcTransport& transport,
        const uint32_t idleTimeoutSeconds)
        : endpointId(endpointId),
          endpointIdHash(endpointIdHash),
          transport(transport),
          stream(transport.getId(), transport),
          hasSeenInitialSpeakerList(false),
          idleTimeoutSeconds(idleTimeoutSeconds),
          createdAt(utils::Time::getAbsoluteTime())
    {
    }

    std::string endpointId;
    size_t endpointIdHash;
    transport::RtcTransport& transport;
    webrtc::WebRtcDataStream stream;
    bool hasSeenInitialSpeakerList;
    const uint32_t idleTimeoutSeconds;
    const uint64_t createdAt;
};

} // namespace bridge
