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
        : _ssrc(ssrc),
          _allocator(packetAllocator),
          _rtpMap(rtpMap),
          _sequenceCounter(0),
          _lastExtendedSequenceNumber(0xFFFFFFFF),
          _lastSentPicId(0xFFFFFFFF),
          _lastSentTl0PicIdx(0xFFFFFFFF),
          _lastSentTimestamp(0),
          _sequenceNumberOffset(0),
          _picIdOffset(0),
          _tl0PicIdxOffset(0),
          _timestampOffset(0),
          _lastRewrittenSsrc(ssrc),
          _needsKeyframe(false),
          _highestSeenExtendedSequenceNumber(0xFFFFFFFF),
          _lastRespondedNackPid(0),
          _lastRespondedNackBlp(0),
          _lastRespondedNackTimestamp(0),
          _lastSendTime(utils::Time::getAbsoluteTime()),
          _markedForDeletion(false),
          _idle(false),
          _isSendingRtpPadding(false)
    {
    }

    void onRtpSent(const uint64_t timestamp)
    {
        _lastSendTime = timestamp;
        _idle = false;
    }

    uint32_t _ssrc;

    std::unique_ptr<codec::OpusEncoder> _opusEncoder;
    memory::PacketPoolAllocator& _allocator;
    const bridge::RtpMap& _rtpMap;

    // This is the highest sent outbound sequence number
    uint32_t _sequenceCounter;

    // These are used by the VP8 forwarder
    uint32_t _lastExtendedSequenceNumber;
    uint32_t _lastSentPicId;
    uint32_t _lastSentTl0PicIdx;
    uint32_t _lastSentTimestamp;
    int64_t _sequenceNumberOffset;
    int32_t _picIdOffset;
    int32_t _tl0PicIdxOffset;
    int64_t _timestampOffset;
    uint32_t _lastRewrittenSsrc;
    bool _needsKeyframe;

    // Used to keep track of offset between inbound and outbound sequence numbers
    uint32_t _highestSeenExtendedSequenceNumber;

    // Store the pid and blp of the last nack that was responded to, to avoid resending
    uint16_t _lastRespondedNackPid;
    uint16_t _lastRespondedNackBlp;
    uint64_t _lastRespondedNackTimestamp;

    utils::Optional<PacketCache*> _packetCache;
    uint64_t _lastSendTime;
    bool _markedForDeletion;
    bool _idle;

    bool _isSendingRtpPadding;
};

} // namespace bridge
