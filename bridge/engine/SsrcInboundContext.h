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
        const bridge::RtpMap& rtpMap,
        transport::RtcTransport* sender,
        uint64_t timestamp)
        : ssrc(ssrc),
          rtpMap(rtpMap),
          sender(sender),
          markNextPacket(true),
          rewriteSsrc(ssrc),
          lastReceivedExtendedSequenceNumber(0),
          packetsProcessed(0),
          lastUnprotectedExtendedSequenceNumber(0),
          activeMedia(false),
          lastReceiveTime(timestamp),
          shouldDropPackets(false),
          inactiveCount(0),
          simulcastLevel(0),
          isSsrcUsed(true)
    {
    }

    void onRtpPacket(const uint64_t timestamp)
    {
        activeMedia = true;
        lastReceiveTime = timestamp;
    }

    const uint32_t ssrc;
    const bridge::RtpMap rtpMap;
    transport::RtcTransport* sender;

    std::unique_ptr<codec::OpusDecoder> opusDecoder;

    bool markNextPacket;
    uint32_t rewriteSsrc;
    uint32_t lastReceivedExtendedSequenceNumber;
    uint32_t packetsProcessed;
    uint32_t lastUnprotectedExtendedSequenceNumber;
    bool activeMedia;
    std::atomic_uint64_t lastReceiveTime;

    /** If an inbound stream is considered unstable, we can in a simulcast scenario decide to drop an inbound stream
     * early to avoid toggling between quality levels. If this is set to true, any incoming packets will be dropped. */
    bool shouldDropPackets;

    /** The number of times this inbound stream has transitioned from active to inactive. Used to decide
     * _shouldDropPackets. */
    uint32_t inactiveCount;

    std::shared_ptr<VideoMissingPacketsTracker> videoMissingPacketsTracker;

    PliScheduler pliScheduler;
    uint32_t simulcastLevel;
    std::atomic_bool isSsrcUsed; // for early discarding of video
    std::atomic_size_t endpointIdHash;
};

} // namespace bridge
