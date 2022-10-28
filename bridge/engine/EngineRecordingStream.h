#pragma once
#include "bridge/engine/RecordingOutboundContext.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "transport/RecordingTransport.h"
#include <atomic>
#include <cstdint>

namespace bridge
{

struct EngineRecordingStream
{
    EngineRecordingStream(const std::string& id,
        const size_t endpointIdHash,
        bool isAudioEnabled,
        bool isVideoEnabled,
        bool isScreenSharingEnabled,
        PacketCache& recordingEventPacketCache)
        : id(id),
          endpointIdHash(endpointIdHash),
          transports(2),
          isAudioEnabled(isAudioEnabled),
          isVideoEnabled(isVideoEnabled),
          isScreenSharingEnabled(isScreenSharingEnabled),
          isReady(false),
          ssrcOutboundContexts(1024),
          outboundContextsFinalizerCounter(1024),
          recordingEventsOutboundContext(recordingEventPacketCache),
          recEventUnackedPacketsTracker(2)
    {
    }

    const std::string id;
    const size_t endpointIdHash;
    concurrency::MpmcHashmap32<size_t, transport::RecordingTransport&> transports;
    bool isAudioEnabled;
    bool isVideoEnabled;
    bool isScreenSharingEnabled;
    bool isReady;

    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;
    concurrency::MpmcHashmap32<uint32_t, std::atomic_uint32_t> outboundContextsFinalizerCounter;
    RecordingOutboundContext recordingEventsOutboundContext;

    // missing packet trackers per transport peer
    concurrency::MpmcHashmap32<size_t, UnackedPacketsTracker&> recEventUnackedPacketsTracker;
};

} // namespace bridge
