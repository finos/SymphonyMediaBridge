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
        jobmanager::JobManager& jobManager,
        std::atomic_uint32_t& jobsCounter,
        PacketCache& recordingEventPacketCache)
        : _id(id),
          _endpointIdHash(endpointIdHash),
          _transports(2),
          _isAudioEnabled(isAudioEnabled),
          _isVideoEnabled(isVideoEnabled),
          _isScreenSharingEnabled(isScreenSharingEnabled),
          _isReady(false),
          _ssrcOutboundContexts(1024),
          _recordingEventsOutboundContext(recordingEventPacketCache),
          _serialJobManager(jobManager),
          _jobsCounter(jobsCounter),
          _recEventUnackedPacketsTracker(2)
    {
    }

    std::string _id;
    size_t _endpointIdHash;
    concurrency::MpmcHashmap32<size_t, transport::RecordingTransport&> _transports;
    bool _isAudioEnabled;
    bool _isVideoEnabled;
    bool _isScreenSharingEnabled;
    bool _isReady;

    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> _ssrcOutboundContexts;
    RecordingOutboundContext _recordingEventsOutboundContext;

    jobmanager::SerialJobManager _serialJobManager;
    std::atomic_uint32_t& _jobsCounter;

    // missing packet trackers per transport peer
    concurrency::MpmcHashmap32<size_t, UnackedPacketsTracker&> _recEventUnackedPacketsTracker;
};

} // namespace bridge
