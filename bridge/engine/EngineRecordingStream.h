#pragma once
#include "bridge/engine/PacketCache.h"
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
        std::unique_ptr<PacketCache>&& recordingEventPacketCache)
        : id(id),
          endpointIdHash(endpointIdHash),
          transports(2),
          isAudioEnabled(isAudioEnabled),
          isVideoEnabled(isVideoEnabled),
          isScreenSharingEnabled(isScreenSharingEnabled),
          isReady(false),
          ssrcOutboundContexts(1024),
          recEventUnackedPacketsTracker(2),
          recordingEventPacketCache(std::move(recordingEventPacketCache)),
          recordingEventsOutboundContext(*this->recordingEventPacketCache)
    {
    }

    std::string id;
    size_t endpointIdHash;
    concurrency::MpmcHashmap32<size_t, transport::RecordingTransport&> transports;
    bool isAudioEnabled;
    bool isVideoEnabled;
    bool isScreenSharingEnabled;
    bool isReady;

    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    // missing packet trackers per transport peer
    concurrency::MpmcHashmap32<size_t, UnackedPacketsTracker&> recEventUnackedPacketsTracker;

    std::unique_ptr<PacketCache> recordingEventPacketCache;
    RecordingOutboundContext recordingEventsOutboundContext;
};

} // namespace bridge
