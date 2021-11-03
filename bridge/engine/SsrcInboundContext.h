#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/PliScheduler.h"
#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "codec/OpusDecoder.h"
#include "jobmanager/JobQueue.h"
#include "transport/RtpReceiveState.h"
#include "utils/Optional.h"
#include <cstdint>
#include <memory>

namespace transport
{
class RtcTransport;
}

namespace jobmanager
{
class JobManager;
}

namespace bridge
{

struct RtpMap;

/**
 * Maintains state and media graph for an inbound SSRC media stream
 */
class SsrcInboundContext
{
public:
    SsrcInboundContext(const uint32_t ssrc,
        jobmanager::JobManager& jobManager,
        const bridge::RtpMap& rtpMap,
        const int32_t audioLevelExtensionId,
        transport::RtcTransport* sender,
        uint64_t timestamp)
        : _ssrc(ssrc),
          _rtpMap(rtpMap),
          _audioLevelExtensionId(audioLevelExtensionId),
          _sender(sender),
          _markNextPacket(true),
          _rewriteSsrc(ssrc),
          _lastReceivedExtendedSequenceNumber(0),
          _packetsProcessed(0),
          _lastUnprotectedExtendedSequenceNumber(0),
          _activeMedia(false),
          _lastReceiveTime(timestamp),
          _markedForDeletion(false),
          _idle(false),
          _shouldDropPackets(false),
          _inactiveCount(0)
    {
    }

    void onRtpPacket(const uint64_t timestamp)
    {
        _activeMedia = true;
        _lastReceiveTime = timestamp;
        _idle = false;
    }

    uint32_t _ssrc;
    const bridge::RtpMap _rtpMap;
    int32_t _audioLevelExtensionId;
    transport::RtcTransport* _sender;
    utils::Optional<uint32_t> _rtxSsrc; // points to main from rtc and to rtx from main

    std::unique_ptr<codec::OpusDecoder> _opusDecoder;

    bool _markNextPacket;
    uint32_t _rewriteSsrc;
    uint32_t _lastReceivedExtendedSequenceNumber;
    uint32_t _packetsProcessed;
    uint32_t _lastUnprotectedExtendedSequenceNumber;
    bool _activeMedia;
    uint64_t _lastReceiveTime;
    bool _markedForDeletion;
    bool _idle;

    /** If an inbound stream is considered unstable, we can in a simulcast scenario decide to drop an inbound stream
     * early to avoid toggling between quality levels. If this is set to true, any incoming packets will be dropped. */
    bool _shouldDropPackets;

    /** The number of times this inbound stream has transitioned from active to inactive. Used to decide
     * _shouldDropPackets. */
    uint32_t _inactiveCount;

    std::shared_ptr<VideoMissingPacketsTracker> _videoMissingPacketsTracker;

    PliScheduler _pliScheduler;
};

} // namespace bridge
