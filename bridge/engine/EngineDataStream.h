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
    EngineDataStream(const std::string& endpointId_,
        const size_t endpointIdHash_,
        transport::RtcTransport& transport_,
        const uint32_t idleTimeoutSeconds_)
        : endpointId(endpointId_),
          endpointIdHash(endpointIdHash_),
          transport(transport_),
          stream(transport.getId(), transport),
          hasSeenInitialSpeakerList(false),
          idleTimeoutSeconds(idleTimeoutSeconds_),
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
