#pragma once
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"

namespace bridge
{
namespace AudioRewriter
{
// no reason to emulate packet loss on our outgoing link. It could also be a long mute.
const int32_t MAX_SEQ_GAP = 256;

inline void rewrite(bridge::SsrcOutboundContext& outboundContext,
    const uint32_t sequenceNumber,
    rtp::RtpHeader& header,
    const uint64_t timestamp)
{
    const uint32_t originalSsrc = header.ssrc;
    const bool newSource = outboundContext.originalSsrc != originalSsrc;
    const int32_t seqAdvance = (sequenceNumber + outboundContext.rewrite.offset.sequenceNumber) -
        outboundContext.rewrite.lastSent.sequenceNumber;

    if (newSource)
    {
        if (outboundContext.rewrite.lastSent.empty())
        {
            outboundContext.rewrite.lastSent.wallClock = timestamp;
            outboundContext.rewrite.lastSent.timestamp = header.timestamp;
            outboundContext.rewrite.lastSent.sequenceNumber = sequenceNumber - 1;
        }
        const uint32_t projectedRtpTimestamp = outboundContext.rewrite.lastSent.timestamp +
            (timestamp - outboundContext.rewrite.lastSent.wallClock) * codec::Opus::sampleRate / utils::Time::sec;

        outboundContext.rewrite.offset.timestamp = projectedRtpTimestamp - header.timestamp.get();
        outboundContext.rewrite.offset.sequenceNumber =
            outboundContext.rewrite.lastSent.sequenceNumber + 1 - sequenceNumber;
        outboundContext.rewrite.sequenceNumberStart = sequenceNumber;
    }
    else if (seqAdvance > MAX_SEQ_GAP)
    {
        const uint32_t projectedRtpTimestamp = outboundContext.rewrite.lastSent.timestamp +
            (timestamp - outboundContext.rewrite.lastSent.wallClock) * codec::Opus::sampleRate / utils::Time::sec;

        outboundContext.rewrite.offset.sequenceNumber =
            outboundContext.rewrite.lastSent.sequenceNumber + 1 - sequenceNumber;
        outboundContext.rewrite.offset.timestamp = projectedRtpTimestamp - header.timestamp.get();
    }

    const uint32_t newSequenceNumber = sequenceNumber + outboundContext.rewrite.offset.sequenceNumber;
    header.sequenceNumber = newSequenceNumber & 0xFFFFu;
    header.ssrc = outboundContext.ssrc;
    header.timestamp = outboundContext.rewrite.offset.timestamp + header.timestamp;
    header.payloadType = outboundContext.rtpMap.payloadType;

    outboundContext.originalSsrc = originalSsrc;
    if (static_cast<int32_t>(newSequenceNumber - outboundContext.rewrite.lastSent.sequenceNumber) > 0)
    {
        outboundContext.rewrite.lastSent.sequenceNumber = newSequenceNumber;
        outboundContext.rewrite.lastSent.timestamp = header.timestamp;
        outboundContext.rewrite.lastSent.wallClock = timestamp;
    }
}
} // namespace AudioRewriter
} // namespace bridge
