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

inline void rewrite(bridge::SsrcOutboundContext& outboundContext, const uint32_t sequenceNumber, rtp::RtpHeader& header)
{
    const uint32_t originalSsrc = header.ssrc;
    const bool newSource = outboundContext.originalSsrc != originalSsrc;
    const int32_t seqAdvance = static_cast<int32_t>((sequenceNumber + outboundContext.rewrite.offset.sequenceNumber) -
        outboundContext.rewrite.lastSent.sequenceNumber);

    if (newSource)
    {
        outboundContext.rewrite.offset.timestamp = outboundContext.rewrite.lastSent.timestamp +
            codec::Opus::sampleRate / codec::Opus::packetsPerSecond - header.timestamp.get();
        outboundContext.rewrite.offset.sequenceNumber =
            static_cast<int32_t>(outboundContext.rewrite.lastSent.sequenceNumber + 1 - sequenceNumber);
        outboundContext.rewrite.sequenceNumberStart = sequenceNumber;
    }
    else if (seqAdvance > MAX_SEQ_GAP)
    {
        // leave timestamp offset as is
        outboundContext.rewrite.offset.sequenceNumber =
            static_cast<int32_t>(outboundContext.rewrite.lastSent.sequenceNumber + 1 - sequenceNumber);
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
    }
}
} // namespace AudioRewriter
} // namespace bridge
