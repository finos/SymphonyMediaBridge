#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/PliScheduler.h"
#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "codec/AudioReceivePipeline.h"
#include "codec/OpusDecoder.h"
#include "jobmanager/JobQueue.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include <cstdint>
#include <memory>

namespace jobmanager
{
class JobManager;
}

namespace codec
{
class AudioReceivePipeline;
}

namespace bridge
{

struct RtpMap;

/**
 * Maintains state and media graph for an inbound SSRC media stream
 */
class SsrcInboundContext
{
    SsrcInboundContext(const uint32_t ssrc,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& telephoneEventRtpMap,
        transport::RtcTransport* sender,
        uint64_t timestamp,
        uint32_t simulcastLevel,
        uint32_t defaultLevelSsrc)
        : ssrc(ssrc),
          rtpMap(rtpMap),
          telephoneEventRtpMap(telephoneEventRtpMap),
          sender(sender),
          simulcastLevel(simulcastLevel),
          defaultLevelSsrc(defaultLevelSsrc),
          markNextPacket(true),
          lastReceivedExtendedSequenceNumber(0),
          packetsProcessed(0),
          lastUnprotectedExtendedSequenceNumber(0),
          activeMedia(false),
          inactiveTransitionCount(0),
          isSsrcUsed(true),
          endpointIdHash(sender ? sender->getEndpointIdHash() : 0),
          shouldDropPackets(false),
          hasAudioLevelExtension(true),
          opusDecodePacketRate(0),
          hasAudioReceivePipe(false),
          _lastRtpReceiveTime(timestamp)
    {
    }

public:
    SsrcInboundContext(const uint32_t ssrc,
        const bridge::RtpMap& rtpMap,
        transport::RtcTransport* sender,
        uint64_t timestamp,
        uint32_t simulcastLevel,
        uint32_t defaultLevelSsrc)
        : SsrcInboundContext(ssrc, rtpMap, bridge::RtpMap(), sender, timestamp, simulcastLevel, defaultLevelSsrc)
    {
    }

    SsrcInboundContext(const uint32_t ssrc,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& telephoneEventRtpMap,
        transport::RtcTransport* sender,
        uint64_t timestamp)
        : SsrcInboundContext(ssrc, rtpMap, telephoneEventRtpMap, sender, timestamp, 0, 0)
    {
    }

    // ensure no copies on engine thread
    SsrcInboundContext(const SsrcInboundContext&) = delete;
    SsrcInboundContext& operator=(const SsrcInboundContext&) = delete;

    void onRtpPacketReceived(const uint64_t timestamp) { _lastRtpReceiveTime = timestamp; }
    bool hasRecentActivity(const uint64_t intervalNs, const uint64_t timestamp)
    {
        return utils::Time::diffLT(_lastRtpReceiveTime.load(), timestamp, intervalNs);
    }

    // make ready for reactivation
    void makeReady()
    {
        inactiveTransitionCount = 0;
        activeMedia = false;
        isSsrcUsed = true;
        shouldDropPackets = false;
    }

    const uint32_t ssrc;
    const bridge::RtpMap rtpMap;
    const bridge::RtpMap telephoneEventRtpMap;
    transport::RtcTransport* const sender;
    const uint32_t simulcastLevel;
    const uint32_t defaultLevelSsrc; // default level for simulcast stream

    // transport thread variables ===================================
    bool markNextPacket;
    uint32_t lastReceivedExtendedSequenceNumber;
    uint32_t packetsProcessed;
    uint32_t lastUnprotectedExtendedSequenceNumber;
    std::shared_ptr<VideoMissingPacketsTracker> videoMissingPacketsTracker;
    std::unique_ptr<codec::OpusDecoder> opusDecoder; // used for missing audio level
    std::unique_ptr<utils::AvgRateTracker> opusPacketRate; // pkt/s

    // engine variables ==============================================
    bool activeMedia;
    uint32_t inactiveTransitionCount; // used to decide shouldDropPackets and turn this simulcast level off

    // engine + transport thread access =============================
    std::atomic_bool isSsrcUsed; // for early discarding of video
    std::atomic_size_t endpointIdHash; // current remote endpoint. Changes for barbelled streams
    PliScheduler pliScheduler; // mainly transport, trigger by engine
    /** If an inbound stream is considered unstable, we can, in a simulcast scenario, decide to drop an inbound stream
     * early to avoid toggling between quality levels. If this is set to true, all incoming packets will be dropped. */
    std::atomic_bool shouldDropPackets;
    std::atomic_bool hasAudioLevelExtension;
    std::atomic<double> opusDecodePacketRate;

    std::unique_ptr<codec::AudioReceivePipeline> audioReceivePipe;
    std::atomic_bool hasAudioReceivePipe;

private:
    std::atomic_uint64_t _lastRtpReceiveTime;
};

} // namespace bridge
