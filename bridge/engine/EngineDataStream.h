#pragma once

#include "transport/RtcTransport.h"
#include "webrtc/WebRtcDataStream.h"
#include "bridge/engine/UntypedEngineObject.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <unistd.h>

namespace bridge
{

struct EngineDataStream final : UntypedEngineObject
{
    EngineDataStream(const std::string& endpointId, const size_t endpointIdHash, transport::RtcTransport& transport)
        : endpointId(endpointId),
          endpointIdHash(endpointIdHash),
          transport(transport),
          stream(transport.getId(), transport),
          hasSeenInitialSpeakerList(false)
    {
    }

    std::string endpointId;
    size_t endpointIdHash;
    transport::RtcTransport& transport;
    webrtc::WebRtcDataStream stream;
    bool hasSeenInitialSpeakerList;
};

} // namespace bridge
