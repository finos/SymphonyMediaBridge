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

class PacketCache;

/**
 * Maintains state and media graph for an outbound SSRC stream.
 */
class SsrcOutboundContext
{
public:
    SsrcOutboundContext(const uint32_t ssrc, memory::PacketPoolAllocator& packetAllocator, const bridge::RtpMap& rtpMap)
        : ssrc(ssrc),
          allocator(packetAllocator),
          rtpMap(rtpMap),
          needsKeyframe(false),
          lastKeyFrameSequenceNumber(0),
          lastRespondedNackPid(0),
          lastRespondedNackBlp(0),
          lastRespondedNackTimestamp(0),
          originalSsrc(~0u),
          lastSendTime(utils::Time::getAbsoluteTime()),
          markedForDeletion(false),
          recordingOutboundDecommissioned(false)
    {
    }

    const uint32_t ssrc;
    memory::PacketPoolAllocator& allocator;
    const bridge::RtpMap& rtpMap;

    // the following are access only from Transport Jobs
    std::unique_ptr<codec::OpusEncoder> opusEncoder;

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
                    ~0u == timestamp && wallClock == ~0u;
            }

            uint32_t ssrc = ~0u;
            uint32_t sequenceNumber = ~0u;
            uint32_t timestamp = ~0u;
            uint16_t picId = -1;
            uint16_t tl0PicIdx = -1;
            uint64_t wallClock = ~0u;
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
    } rewrite;

    bool shouldSend(uint32_t ssrc, uint32_t extendedSequenceNumber) const;

    bool needsKeyframe;
    uint32_t lastKeyFrameSequenceNumber;

    // Store the pid and blp of the last nack that was responded to, to avoid resending
    uint16_t lastRespondedNackPid;
    uint16_t lastRespondedNackBlp;
    uint64_t lastRespondedNackTimestamp;

    utils::Optional<PacketCache*> packetCache;

    /// ==== both Engine and Transport
    std::atomic_uint32_t originalSsrc;

    /// ==== Accessed from Engine only!
    uint64_t lastSendTime;
    void onRtpSent(const uint64_t timestamp) { lastSendTime = timestamp; }

    // Stream owner is being removed. Stop outbound packets over this context
    bool markedForDeletion;

    // Retain rec OutboundSsrc before marking for deletion to sustain retransmissions longer.
    bool recordingOutboundDecommissioned;
};

} // namespace bridge
