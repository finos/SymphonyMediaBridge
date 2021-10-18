#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "logger/Logger.h"
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

constexpr uint32_t applyOffset(const uint32_t extendedSequenceNumber, const int64_t offset)
{
    const auto result = static_cast<int64_t>(extendedSequenceNumber) + offset;
    if (result < 0)
    {
        return static_cast<uint32_t>(result) & 0xFFFF;
    }

    return static_cast<uint32_t>(result);
}

constexpr uint16_t extractSequenceNumber(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber & 0xFFFF);
}

constexpr uint16_t extractRolloverCounter(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber >> 16);
}

inline bool rewrite(SsrcOutboundContext& ssrcOutboundContext,
    memory::Packet* rewritePacket,
    const uint32_t rewriteSsrc,
    const uint32_t extendedSequenceNumber,
    const char* transportName,
    uint32_t& outExtendedSequenceNumber)
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*rewritePacket);
    if (!rtpHeader)
    {
        return false;
    }

    auto rtpPayload = rtpHeader->getPayload();
    const auto timestamp = rtpHeader->timestamp.get();
    const auto ssrc = rtpHeader->ssrc.get();
    const auto picId = codec::Vp8Header::getPicId(rtpPayload);
    const auto tl0PicIdx = codec::Vp8Header::getTl0PicIdx(rtpPayload);

    if (ssrcOutboundContext._lastExtendedSequenceNumber == 0xFFFFFFFF)
    {
        ssrcOutboundContext._lastExtendedSequenceNumber = extendedSequenceNumber;
        ssrcOutboundContext._lastSentPicId = codec::Vp8Header::getPicId(rtpPayload);
        ssrcOutboundContext._lastSentTl0PicIdx = codec::Vp8Header::getTl0PicIdx(rtpPayload);
        ssrcOutboundContext._lastSentTimestamp = timestamp;
        logger::info("Ssrc %u -> %u, sequence %u roc %u",
            "Vp8Rewriter",
            rtpHeader->ssrc.get(),
            rewriteSsrc,
            extractSequenceNumber(extendedSequenceNumber),
            extractRolloverCounter(extendedSequenceNumber));
    }

    if (ssrcOutboundContext._lastRewrittenSsrc != ssrc)
    {
        ssrcOutboundContext._lastRewrittenSsrc = ssrc;
        ssrcOutboundContext._sequenceNumberOffset = utils::Offset::getOffset<int64_t, 32>(
            static_cast<int32_t>(ssrcOutboundContext._lastExtendedSequenceNumber + 1),
            static_cast<int64_t>(extendedSequenceNumber));

        ssrcOutboundContext._picIdOffset =
            utils::Offset::getOffset<int32_t, 15>(static_cast<int32_t>(ssrcOutboundContext._lastSentPicId + 1),
                static_cast<int32_t>(picId));

        ssrcOutboundContext._tl0PicIdxOffset =
            utils::Offset::getOffset<int32_t, 8>(static_cast<int32_t>(ssrcOutboundContext._lastSentTl0PicIdx + 1),
                static_cast<int32_t>(tl0PicIdx));

        ssrcOutboundContext._timestampOffset =
            utils::Offset::getOffset<int64_t, 32>(static_cast<int64_t>(ssrcOutboundContext._lastSentTimestamp + 1),
                static_cast<int64_t>(timestamp)) +
            500;

#if DEBUG_REWRITER
        logger::debug("New offset, %s ssrc %u, oseq %" PRIi64 ", oPicId %d, otl0PicIdx %d, oTimestamp %" PRIi64,
            "Vp8Rewriter",
            transportName,
            rtpHeader->ssrc.get(),
            ssrcOutboundContext._sequenceNumberOffset,
            ssrcOutboundContext._picIdOffset,
            ssrcOutboundContext._tl0PicIdxOffset,
            ssrcOutboundContext._timestampOffset);
#endif
        logger::info("Ssrc %u -> %u, sequence %u",
            "Vp8Rewriter",
            rtpHeader->ssrc.get(),
            rewriteSsrc,
            applyOffset(extendedSequenceNumber, ssrcOutboundContext._sequenceNumberOffset));
    }

    const auto newExtendedSequenceNumber =
        applyOffset(extendedSequenceNumber, ssrcOutboundContext._sequenceNumberOffset);
    const auto newPicId = static_cast<uint16_t>(static_cast<int32_t>(picId) + ssrcOutboundContext._picIdOffset);
    const auto newTl0PicIdx =
        static_cast<uint8_t>(static_cast<int32_t>(tl0PicIdx) + ssrcOutboundContext._tl0PicIdxOffset);
    const auto newTimestamp =
        static_cast<uint32_t>(static_cast<int64_t>(timestamp) + ssrcOutboundContext._timestampOffset);

    rtpHeader->ssrc = rewriteSsrc;
    rtpHeader->sequenceNumber = newExtendedSequenceNumber;
    rtpHeader->timestamp = newTimestamp;
    codec::Vp8Header::setPicId(rtpPayload, newPicId);
    codec::Vp8Header::setTl0PicIdx(rtpPayload, newTl0PicIdx);

#if DEBUG_REWRITER
    logger::debug(
        "Fwd, %s ssrc %u -> %u, seq %u (%u) -> %u (%u), marker %u, picId %d -> %d, tl0PicIdx %d -> %d, ts %u -> %u",
        "Vp8Rewriter",
        transportName,
        ssrc,
        rtpHeader->ssrc.get(),
        extractSequenceNumber(extendedSequenceNumber),
        extractRolloverCounter(extendedSequenceNumber),
        extractSequenceNumber(newExtendedSequenceNumber),
        extractRolloverCounter(newExtendedSequenceNumber),
        rtpHeader->marker,
        picId,
        newPicId,
        tl0PicIdx,
        newTl0PicIdx,
        timestamp,
        newTimestamp);
#endif

    if (newExtendedSequenceNumber > ssrcOutboundContext._lastExtendedSequenceNumber)
    {
        ssrcOutboundContext._lastExtendedSequenceNumber = newExtendedSequenceNumber;
        ssrcOutboundContext._lastSentPicId = newPicId;
        ssrcOutboundContext._lastSentTl0PicIdx = newTl0PicIdx;
        ssrcOutboundContext._lastSentTimestamp = newTimestamp;
    }

    outExtendedSequenceNumber = newExtendedSequenceNumber;
    return true;
}

inline uint16_t rewriteRtxPacket(memory::Packet& packet, const uint32_t mainSsrc)
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
    logger::debug("rewriteRtxPacket ssrc %u -> %u, seq %u -> %u",
        "Vp8Rewriter",
        rtpHeader->ssrc.get(),
        mainSsrc,
        rtpHeader->sequenceNumber.get(),
        originalSequenceNumber);
#endif

    rtpHeader->sequenceNumber = originalSequenceNumber;
    rtpHeader->ssrc = mainSsrc;

    return originalSequenceNumber;
}

} // namespace Vp8Rewriter

} // namespace bridge
