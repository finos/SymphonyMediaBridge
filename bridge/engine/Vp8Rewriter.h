#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "logger/Logger.h"
#include "math/Fields.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"
#include "utils/Offset.h"
#include <cassert>
#include <cstddef>
#include <cstdint>

#define DEBUG_REWRITER 0

namespace bridge
{

namespace Vp8Rewriter
{
constexpr int32_t MAX_JUMP_AHEAD = 0x10000 / 4;

constexpr uint16_t extractSequenceNumber(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber & 0xFFFF);
}

constexpr uint16_t extractRolloverCounter(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber >> 16);
}

inline bool rewrite(SsrcOutboundContext& ssrcOutboundContext,
    memory::Packet& rewritePacket,
    const uint32_t extendedSequenceNumber,
    const char* transportName,
    uint32_t& outExtendedSequenceNumber,
    bool isKeyFrame = false)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(rewritePacket);
    if (!rtpHeader)
    {
        assert(false);
        return false;
    }

    const auto rtpPayload = rtpHeader->getPayload();
    const auto timestamp = rtpHeader->timestamp.get();
    const auto ssrc = rtpHeader->ssrc.get();
    const auto picId = codec::Vp8Header::getPicId(rtpPayload);
    const auto tl0PicIdx = codec::Vp8Header::getTl0PicIdx(rtpPayload);

    auto& ssrcRewrite = ssrcOutboundContext.rewrite;
    if (ssrcRewrite.empty())
    {
        ssrcRewrite.originalSsrc = ssrc - 1;
        ssrcRewrite.lastSent.sequenceNumber = extendedSequenceNumber - 1;
        ssrcRewrite.lastSent.picId = (codec::Vp8Header::getPicId(rtpPayload) - 1) & 0x7FFF;
        ssrcRewrite.lastSent.tl0PicIdx = codec::Vp8Header::getTl0PicIdx(rtpPayload) - 1;
        ssrcRewrite.lastSent.timestamp = timestamp;
        logger::info("%s start ssrc %u -> %u, sequence %u roc %u",
            "Vp8Rewriter",
            transportName,
            rtpHeader->ssrc.get(),
            ssrcOutboundContext.ssrc,
            extractSequenceNumber(extendedSequenceNumber),
            extractRolloverCounter(extendedSequenceNumber));
    }

    if (ssrcRewrite.originalSsrc != ssrc)
    {
        ssrcRewrite.originalSsrc = ssrc;
        ssrcRewrite.offset.sequenceNumber =
            math::ringDifference<uint32_t, 32>(extendedSequenceNumber, ssrcRewrite.lastSent.sequenceNumber + 1);
        ssrcRewrite.sequenceNumberStart = extendedSequenceNumber;

        ssrcRewrite.offset.picId = math::ringDifference<uint16_t, 15>(picId, ssrcRewrite.lastSent.picId + 1);

        ssrcRewrite.offset.tl0PicIdx = math::ringDifference<uint16_t, 8>(tl0PicIdx, ssrcRewrite.lastSent.tl0PicIdx + 1);

        ssrcRewrite.offset.timestamp =
            math::ringDifference<uint32_t, 32>(timestamp, ssrcRewrite.lastSent.timestamp + 500);

#if DEBUG_REWRITER
        logger::debug("%s new offset, ssrc %u, oseq %d, oPicId %d, otl0PicIdx %d, oTimestamp %d",
            "Vp8Rewriter",
            transportName,
            rtpHeader->ssrc.get(),
            ssrcRewrite.offset.sequenceNumber,
            ssrcRewrite.offset.picId,
            ssrcRewrite.offset.tl0PicIdx,
            ssrcRewrite.offset.timestamp);
#endif
        logger::info("%s ssrc %u -> %u, sequence %u",
            "Vp8Rewriter",
            transportName,
            rtpHeader->ssrc.get(),
            ssrcOutboundContext.ssrc,
            extendedSequenceNumber + ssrcRewrite.offset.sequenceNumber);
    }
    else if (static_cast<int32_t>(extendedSequenceNumber + ssrcRewrite.offset.sequenceNumber -
                 ssrcRewrite.lastSent.sequenceNumber) > MAX_JUMP_AHEAD)
    {
        ssrcRewrite.offset.sequenceNumber =
            math::ringDifference<uint32_t, 32>(extendedSequenceNumber, ssrcRewrite.lastSent.sequenceNumber + 1);
        logger::debug("Major sequence number skip ssrc %u, seq %u, sent %u. Adjusting offset to hide it",
            "Vp8Rewriter",
            rtpHeader->ssrc.get(),
            extendedSequenceNumber,
            ssrcRewrite.lastSent.sequenceNumber);
    }

    outExtendedSequenceNumber = extendedSequenceNumber + ssrcRewrite.offset.sequenceNumber;
    const auto newPicId = picId + ssrcRewrite.offset.picId;
    const auto newTl0PicIdx = tl0PicIdx + ssrcRewrite.offset.tl0PicIdx;
    const auto newTimestamp = timestamp + ssrcRewrite.offset.timestamp;

    rtpHeader->ssrc = ssrcOutboundContext.ssrc;
    rtpHeader->sequenceNumber = outExtendedSequenceNumber & 0xFFFFu;
    rtpHeader->timestamp = newTimestamp;
    codec::Vp8Header::setPicId(rtpPayload, newPicId);
    codec::Vp8Header::setTl0PicIdx(rtpPayload, newTl0PicIdx);

#if DEBUG_REWRITER
    logger::debug(
        "%s fwd ssrc %u -> %u, seq %u (%u) -> %u (%u), marker %u, picId %d -> %d, tl0PicIdx %d -> %d, ts %u -> %u",
        "Vp8Rewriter",
        transportName,
        ssrc,
        rtpHeader->ssrc.get(),
        extractSequenceNumber(extendedSequenceNumber),
        extractRolloverCounter(extendedSequenceNumber),
        extractSequenceNumber(outExtendedSequenceNumber),
        extractRolloverCounter(outExtendedSequenceNumber),
        rtpHeader->marker,
        picId,
        newPicId,
        tl0PicIdx,
        newTl0PicIdx,
        timestamp,
        newTimestamp);
#endif

    if (static_cast<int32_t>(outExtendedSequenceNumber - ssrcRewrite.lastSent.sequenceNumber) > 0)
    {
        ssrcRewrite.lastSent.sequenceNumber = outExtendedSequenceNumber;
        ssrcRewrite.lastSent.picId = newPicId;
        ssrcRewrite.lastSent.tl0PicIdx = newTl0PicIdx;
        ssrcRewrite.lastSent.timestamp = newTimestamp;
    }

    if (isKeyFrame)
    {
        ssrcOutboundContext.lastKeyFrameSequenceNumber = outExtendedSequenceNumber;
    }

    return true;
}

inline uint16_t rewriteRtxPacket(memory::Packet& packet,
    const uint32_t mainSsrc,
    uint8_t vp8PayloadType,
    const char* transportName)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
    assert(rtpHeader->padding == 0);

    const auto headerLength = rtpHeader->headerLength();
    const auto payload = rtpHeader->getPayload();

    const auto originalSequenceNumber =
        (static_cast<uint16_t>(payload[0]) << 8) | (static_cast<uint16_t>(payload[1]) & 0xFF);

    memmove(payload, payload + sizeof(uint16_t), packet.getLength() - headerLength - sizeof(uint16_t));
    packet.setLength(packet.getLength() - sizeof(uint16_t));

#if DEBUG_REWRITER
    logger::debug("%s rewriteRtxPacket ssrc %u -> %u, seq %u -> %u",
        "Vp8Rewriter",
        transportName,
        rtpHeader->ssrc.get(),
        mainSsrc,
        rtpHeader->sequenceNumber.get(),
        originalSequenceNumber);
#endif

    rtpHeader->sequenceNumber = originalSequenceNumber;
    rtpHeader->ssrc = mainSsrc;
    rtpHeader->payloadType = vp8PayloadType;

    return originalSequenceNumber;
}

} // namespace Vp8Rewriter

} // namespace bridge
