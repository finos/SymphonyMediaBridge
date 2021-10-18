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
        memory::PacketPoolAllocator& allocator)
        : _endpointId(endpointId),
          _endpointIdHash(endpointIdHash),
          _transport(transport),
          _stream(transport.getId(), transport, allocator),
          _hasSeenInitialSpeakerList(false)
    {
    }

    std::string _endpointId;
    size_t _endpointIdHash;
    transport::RtcTransport& _transport;
    webrtc::WebRtcDataStream _stream;
    bool _hasSeenInitialSpeakerList;
};

} // namespace bridge
