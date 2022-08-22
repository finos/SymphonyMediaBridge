#pragma once

#include "bridge/RtpMap.h"
#include "codec/OpusEncoder.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <atomic>
#include <cstdint>
#include <memory>

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
          sequenceCounter(0),
          lastExtendedSequenceNumber(0xFFFFFFFF),
          lastSentPicId(0xFFFFFFFF),
          lastSentTl0PicIdx(0xFFFFFFFF),
          lastSentTimestamp(0),
          sequenceNumberOffset(0),
          picIdOffset(0),
          tl0PicIdxOffset(0),
          timestampOffset(0),
          lastRewrittenSsrc(ssrc),
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

    uint32_t ssrc;

    std::unique_ptr<codec::OpusEncoder> opusEncoder;
    memory::PacketPoolAllocator& allocator;
    const bridge::RtpMap& rtpMap;

    // This is the highest sent outbound sequence number
    uint32_t sequenceCounter;

    // These are used by the VP8 forwarder
    uint32_t lastExtendedSequenceNumber;
    uint32_t lastSentPicId;
    uint32_t lastSentTl0PicIdx;
    uint32_t lastSentTimestamp;
    int64_t sequenceNumberOffset;
    int32_t picIdOffset;
    int32_t tl0PicIdxOffset;
    int64_t timestampOffset;
    uint32_t lastRewrittenSsrc;
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
    bool markedForDeletion;
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
