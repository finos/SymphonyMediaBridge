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
          highestSeenExtendedSequenceNumber(0xFFFFFFFF),
          lastRespondedNackPid(0),
          lastRespondedNackBlp(0),
          lastRespondedNackTimestamp(0),
          lastSendTime(utils::Time::getAbsoluteTime()),
          markedForDeletion(false),
          idle(false)
    {
    }

    void onRtpSent(const uint64_t timestamp)
    {
        lastSendTime = timestamp;
        idle = false;
    }

    const uint32_t ssrc;

    std::unique_ptr<codec::OpusEncoder> opusEncoder;
    memory::PacketPoolAllocator& allocator;
    const bridge::RtpMap& rtpMap;

    // These are used by the VP8 forwarder
    struct SsrcRewrite
    {
        bool empty() const { return lastSent.empty() && offset.empty(); }
        bool shouldSend(uint32_t ssrc, uint32_t extendedSequenceNumber) const;

        uint32_t originalSsrc = ~0u;
        uint32_t sequenceNumberStart = 0;

        struct Tracker
        {
            bool empty() const
            {
                return ssrc == ~0u && sequenceNumber == ~0u && 0xFFFFu == picId && 0xFFFFu == tl0PicIdx &&
                    ~0u == timestamp;
            }

            uint32_t ssrc = ~0;
            uint32_t sequenceNumber = ~0;
            uint32_t timestamp = ~0;
            uint16_t picId = ~0;
            uint16_t tl0PicIdx = ~0;
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

    bool needsKeyframe;
    uint32_t lastKeyFrameSequenceNumber;

    // Used to keep track of offset between inbound and outbound sequence numbers
    uint32_t highestSeenExtendedSequenceNumber;

    // Store the pid and blp of the last nack that was responded to, to avoid resending
    uint16_t lastRespondedNackPid;
    uint16_t lastRespondedNackBlp;
    uint64_t lastRespondedNackTimestamp;

    utils::Optional<PacketCache*> packetCache;
    uint64_t lastSendTime;

    // Stream owner is being removed. Stop outbound packets over this context
    bool markedForDeletion;

    // Stream is temporarily idle and srtp encryption context is removed. It may be activated again.
    bool idle;

    struct PliMetric
    {
        uint64_t userRequestTimestamp = 0;
        uint64_t triggerTimestamp = 0;
        uint64_t keyFrameTimestamp = 0;

        uint64_t getDelay(const uint64_t timestamp) const
        {
            if (userRequestTimestamp != 0 && static_cast<int64_t>(userRequestTimestamp - keyFrameTimestamp) > 0)
            {
                return timestamp - userRequestTimestamp;
            }
            else
            {
                return timestamp - triggerTimestamp;
            }
        }
    } pli;
};

} // namespace bridge
