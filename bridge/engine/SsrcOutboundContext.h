#pragma once

#include "bridge/RtpMap.h"
#include "codec/OpusEncoder.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <atomic>
#include <cstdint>
#include <memory>

namespace rtp
{
struct RtpHeader;
} // namespace rtp

namespace bridge
{

class SsrcInboundContext;
class PacketCache;

/**
 * Maintains state and media graph for an outbound SSRC stream.
 */
class SsrcOutboundContext
{
public:
    SsrcOutboundContext(const uint32_t ssrc,
        memory::PacketPoolAllocator& packetAllocator,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& telephoneEventRtpMap)
        : ssrc(ssrc),
          allocator(packetAllocator),
          rtpMap(rtpMap),
          telephoneEventRtpMap(telephoneEventRtpMap),
          needsKeyframe(false),
          lastKeyFrameSequenceNumber(0),
          lastRespondedNackPid(0),
          lastRespondedNackBlp(0),
          lastRespondedNackTimestamp(0),
          lastSendTime(utils::Time::getAbsoluteTime()),
          markedForDeletion(false),
          recordingOutboundDecommissioned(false),
          _originalSsrc(~0u)
    {
    }

    void dropTelephoneEvent(const uint32_t sequenceNumber, const uint32_t originSsrc);
    bool rewriteAudio(rtp::RtpHeader& header,
        const bridge::SsrcInboundContext& senderInboundContext,
        const uint32_t sequenceNumber,
        const uint64_t timestamp,
        const bool isTelephoneEvent);
    bool rewriteVideo(rtp::RtpHeader& header,
        const bridge::SsrcInboundContext& senderInboundContext,
        const uint32_t extendedSequenceNumber,
        const char* transportName,
        uint32_t& outExtendedSequenceNumber,
        const uint64_t timestamp,
        bool isKeyFrame);

    uint32_t getLastSentSequenceNumber() const { return _rewrite.lastSent.sequenceNumber; }
    int32_t getSequenceNumberOffset() const { return _rewrite.offset.sequenceNumber; }

    uint32_t& getSequenceNumberReference() { return _rewrite.lastSent.sequenceNumber; }

    uint32_t getOriginalSsrc() const { return _originalSsrc.load(); }

    const uint32_t ssrc;
    memory::PacketPoolAllocator& allocator;
    const bridge::RtpMap& rtpMap;
    const bridge::RtpMap& telephoneEventRtpMap;

    // the following are access only from Transport Jobs
    std::unique_ptr<codec::OpusEncoder> opusEncoder;

    bool needsKeyframe;
    uint32_t lastKeyFrameSequenceNumber;

    // Store the pid and blp of the last nack that was responded to, to avoid resending
    uint16_t lastRespondedNackPid;
    uint16_t lastRespondedNackBlp;
    uint64_t lastRespondedNackTimestamp;

    utils::Optional<PacketCache*> packetCache;

    /// ==== Accessed from Engine only!
    uint64_t lastSendTime;
    void onRtpSent(const uint64_t timestamp) { lastSendTime = timestamp; }

    // Stream owner is being removed. Stop outbound packets over this context
    bool markedForDeletion;

    // Retain rec OutboundSsrc before marking for deletion to sustain retransmissions longer.
    bool recordingOutboundDecommissioned;

private:
    void doRtpHeaderExtensionRewriteForAudio(rtp::RtpHeader& rtpHeader,
        const bridge::SsrcInboundContext& senderInboundContext);
    void doRtpHeaderExtensionRewriteForVideo(rtp::RtpHeader& rtpHeader,
        const bridge::SsrcInboundContext& senderInboundContext);
    void doRtpHeaderRewriteForAudio(rtp::RtpHeader& header,
        const bridge::SsrcInboundContext& senderInboundContext,
        const uint32_t newSequenceNumber,
        const bool isTelephoneEvent);
    void doRtpHeaderRewriteForVideo(rtp::RtpHeader& header,
        const bridge::SsrcInboundContext& senderInboundContext,
        const uint32_t newSequenceNumber);
    bool isPacketTooOld(uint32_t sequenceCount, int32_t rewindLimit) const;

    // These are used by the VP8 forwarder from Transport Jobs only!
    struct SsrcRewrite
    {
        bool empty() const { return lastSent.empty() && offset.empty(); }

        uint32_t sequenceNumberStart = 0;

        struct Tracker
        {
            bool empty() const
            {
                return ssrc == ~0u && sequenceNumber == ~0u && 0xFFFFu == picId && 0xFFFFu == tl0PicIdx &&
                    ~0u == timestamp && wallClock == ~0u && lastOriginalSequenceNumber == ~0u;
            }

            uint32_t ssrc = ~0u;
            uint32_t sequenceNumber = ~0u;
            uint32_t timestamp = ~0u;
            uint64_t wallClock = ~0u;
            uint16_t picId = -1;
            uint16_t tl0PicIdx = -1;
            uint32_t lastOriginalSequenceNumber = ~0;
        } lastSent;

        struct Offset
        {
            bool empty() const
            {
                return sequenceNumber == 0 && picId == 0 && 0 == timestamp && tl0PicIdx == 0 && 0 == sequenceNumber;
            }

            int32_t sequenceNumber = 0;
            int32_t timestamp = 0;
            int16_t picId = 0;
            int16_t tl0PicIdx = 0;
        } offset;

        struct
        {
            uint32_t lastDroppedSequenceNumber = 0;
        } telephoneEvents;
    } _rewrite;

    /// ==== both Engine and Transport
    std::atomic_uint32_t _originalSsrc;
};

} // namespace bridge
