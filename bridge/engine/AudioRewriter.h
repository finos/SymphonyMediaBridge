#pragma once
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/G711.h"
#include "codec/Opus.h"
#include "rtp/RtpHeader.h"

namespace bridge
{
namespace AudioRewriter
{

inline uint32_t clockCyclesPerPacket(bridge::RtpMap::Format format)
{
    if (format == bridge::RtpMap::Format::PCMA || format == bridge::RtpMap::Format::PCMU)
    {
        return codec::Pcma::sampleRate / codec::Pcma::packetsPerSecond;
    }
    else
    {
        return codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
    }
}

inline void rewrite(bridge::SsrcOutboundContext& outboundContext, const uint32_t sequenceNumber, rtp::RtpHeader& header)
{
    const uint32_t originalSsrc = header.ssrc;
    const bool newSource = outboundContext.originalSsrc != originalSsrc;
    if (newSource)
    {
        outboundContext.rewrite.offset.timestamp = outboundContext.rewrite.lastSent.timestamp +
            outboundContext.rtpMap.sampleRate / codec::Opus::packetsPerSecond - header.timestamp.get();
        outboundContext.rewrite.offset.sequenceNumber =
            static_cast<int32_t>(outboundContext.rewrite.lastSent.sequenceNumber + 1 - sequenceNumber);
        outboundContext.rewrite.sequenceNumberStart = sequenceNumber;
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
