#include "bridge/engine/SsrcOutboundContext.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/Vp8.h"
#include "codec/Vp8Header.h"
#include "math/Fields.h"
#include "rtp/RtpHeader.h"

using namespace bridge;

#define DEBUG_REWRITER 0
#if DEBUG_REWRITER
#define REWRITER_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define REWRITER_LOG(fmt, ...) (void)0
#endif

namespace
{
// no reason to emulate packet loss on our outgoing link. It could also be a long mute.
const int32_t MAX_AUDIO_SEQ_GAP = 256;
// if inbound gap is more than this we will keep the outbound sequence monotone
const int32_t MAX_VIDEO_SEQ_GAP = 512;

const int32_t MAX_REWIND_AUDIO = 512;

// for video the packet rate can be higher at key frames, allow more rewind
const int32_t MAX_REWIND_VIDEO = 1024 * 3;

constexpr uint16_t extractSequenceNumber(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber & 0xFFFF);
}

constexpr uint16_t extractRolloverCounter(const uint32_t extendedSequenceNumber)
{
    return static_cast<uint16_t>(extendedSequenceNumber >> 16);
}

} // namespace

void SsrcOutboundContext::doRtpHeaderExtensionRewriteForAudio(rtp::RtpHeader& rtpHeader,
    const bridge::SsrcInboundContext& senderInboundContext)
{
    const bool needToRewriteLevelExtension = senderInboundContext.rtpMap.audioLevelExtId.isSet() &&
        (!rtpMap.audioLevelExtId.isSet() ||
            senderInboundContext.rtpMap.audioLevelExtId.get() != rtpMap.audioLevelExtId.get());

    const bool needToRewriteAbsSendTimeExtension = senderInboundContext.rtpMap.absSendTimeExtId.isSet() &&
        (!rtpMap.absSendTimeExtId.isSet() ||
            senderInboundContext.rtpMap.absSendTimeExtId.get() != rtpMap.absSendTimeExtId.get());

    if (!(needToRewriteLevelExtension || needToRewriteAbsSendTimeExtension))
    {
        return;
    }

    const auto headerExtensions = rtpHeader.getExtensionHeader();
    if (!headerExtensions)
    {
        return;
    }

    for (auto& rtpHeaderExtension : headerExtensions->extensions())
    {
        if (needToRewriteLevelExtension &&
            rtpHeaderExtension.getId() == senderInboundContext.rtpMap.audioLevelExtId.get())
        {
            if (rtpMap.audioLevelExtId.isSet())
            {
                rtpHeaderExtension.setId(rtpMap.audioLevelExtId.get());
            }
            else
            {
                rtpHeaderExtension.fillWithPadding();
            }
        }
        else if (needToRewriteAbsSendTimeExtension &&
            rtpHeaderExtension.getId() == senderInboundContext.rtpMap.absSendTimeExtId.get())
        {
            if (rtpMap.absSendTimeExtId.isSet())
            {
                rtpHeaderExtension.setId(rtpMap.absSendTimeExtId.get());
            }
            else
            {
                rtpHeaderExtension.fillWithPadding();
            }
        }
    }
}

void SsrcOutboundContext::doRtpHeaderExtensionRewriteForVideo(rtp::RtpHeader& rtpHeader,
    const bridge::SsrcInboundContext& senderInboundContext)
{

    const auto headerExtensions = rtpHeader.getExtensionHeader();
    if (!headerExtensions)
    {
        return;
    }

    const bool senderHasAbsSendTimeEx = senderInboundContext.rtpMap.absSendTimeExtId.isSet();
    const bool receiverHasAbsSendTimeEx = rtpMap.absSendTimeExtId.isSet();
    const bool absSendTimeExNeedToBeRewritten = senderHasAbsSendTimeEx && receiverHasAbsSendTimeEx &&
        senderInboundContext.rtpMap.absSendTimeExtId.get() != rtpMap.absSendTimeExtId.get();

    if (absSendTimeExNeedToBeRewritten)
    {
        for (auto& rtpHeaderExtension : headerExtensions->extensions())
        {
            if (rtpHeaderExtension.getId() == senderInboundContext.rtpMap.absSendTimeExtId.get())
            {
                rtpHeaderExtension.setId(rtpMap.absSendTimeExtId.get());
                return;
            }
        }
    }
}

void SsrcOutboundContext::doRtpHeaderRewriteForAudio(rtp::RtpHeader& header,
    const bridge::SsrcInboundContext& senderInboundContext,
    const uint32_t newSequenceNumber,
    const bool isTelephoneEvent)
{
    assert(!(isTelephoneEvent && telephoneEventRtpMap.isEmpty()));

    header.sequenceNumber = newSequenceNumber & 0xFFFFu;
    header.ssrc = this->ssrc;
    header.timestamp = _rewrite.offset.timestamp + header.timestamp;
    header.payloadType = isTelephoneEvent ? telephoneEventRtpMap.payloadType : rtpMap.payloadType;
    doRtpHeaderExtensionRewriteForAudio(header, senderInboundContext);
}

void SsrcOutboundContext::doRtpHeaderRewriteForVideo(rtp::RtpHeader& header,
    const bridge::SsrcInboundContext& senderInboundContext,
    const uint32_t newSequenceNumber)
{
    header.sequenceNumber = newSequenceNumber & 0xFFFFu;
    header.ssrc = this->ssrc;
    header.timestamp = _rewrite.offset.timestamp + header.timestamp;
    header.payloadType = rtpMap.payloadType;
    doRtpHeaderExtensionRewriteForVideo(header, senderInboundContext);
}

bool SsrcOutboundContext::isPacketTooOld(uint32_t sequenceNumber, int32_t rewindLimit) const
{
    const bool packetOlderThanSsrcSwitch = static_cast<int32_t>(sequenceNumber - _rewrite.sequenceNumberStart) < 1;
    if (packetOlderThanSsrcSwitch)
    {
        return true;
    }

    const bool exceedsMaxRewind = (static_cast<int32_t>(sequenceNumber + _rewrite.offset.sequenceNumber -
                                       _rewrite.lastSent.sequenceNumber) <= -rewindLimit);

    if (exceedsMaxRewind)
    {
        return true;
    }

    return false;
}

void SsrcOutboundContext::dropTelephoneEvent(const uint32_t sequenceNumber, const uint32_t originSsrc)
{
    const bool newSource = _originalSsrc != originSsrc;
    if (newSource)
    {
        // Do not change anything in this case, the outbound context will be updated correctly on first audio packet
        return;
    }

    const int32_t seqAdvance = (sequenceNumber + _rewrite.offset.sequenceNumber) - _rewrite.lastSent.sequenceNumber;
    if (seqAdvance > MAX_AUDIO_SEQ_GAP)
    {
        // There is no need to update offsets when the gap is too long. It will be fixed on first audio packet
        return;
    }

    if (seqAdvance > 0)
    {
        _rewrite.offset.sequenceNumber -= seqAdvance;
        _rewrite.telephoneEvents.lastDroppedSequenceNumber = sequenceNumber;
    }
}

bool SsrcOutboundContext::rewriteAudio(rtp::RtpHeader& header,
    const bridge::SsrcInboundContext& senderInboundContext,
    const uint32_t sequenceNumber,
    const uint64_t timestamp,
    const bool isTelephoneEvent)
{
    const uint32_t currentSsrc = header.ssrc;

    if (isTelephoneEvent && telephoneEventRtpMap.isEmpty())
    {
        dropTelephoneEvent(sequenceNumber, currentSsrc);
        return false;
    }

    const bool newSource = currentSsrc != _originalSsrc;
    const int32_t seqAdvance = (sequenceNumber + _rewrite.offset.sequenceNumber) - _rewrite.lastSent.sequenceNumber;

    if (newSource)
    {
        if (_rewrite.lastSent.empty())
        {
            _rewrite.lastSent.wallClock = timestamp;
            _rewrite.lastSent.timestamp = header.timestamp;
            _rewrite.lastSent.sequenceNumber = sequenceNumber - 1;
            _rewrite.lastSent.lastOriginalSequenceNumber = sequenceNumber - 1;
        }
        const uint32_t projectedRtpTimestamp = _rewrite.lastSent.timestamp +
            (timestamp - _rewrite.lastSent.wallClock) * codec::Opus::sampleRate / utils::Time::sec;

        _rewrite.offset.timestamp = projectedRtpTimestamp - header.timestamp.get();
        _rewrite.offset.sequenceNumber = _rewrite.lastSent.sequenceNumber + 1 - sequenceNumber;
        _rewrite.sequenceNumberStart = sequenceNumber;

        // mark each time the ssrc as there was an disruption on transmission on this ssrc
        header.marker = 1;
    }
    else if (seqAdvance > MAX_AUDIO_SEQ_GAP)
    {
        const uint32_t projectedRtpTimestamp = _rewrite.lastSent.timestamp +
            (timestamp - _rewrite.lastSent.wallClock) * codec::Opus::sampleRate / utils::Time::sec;

        _rewrite.offset.sequenceNumber = _rewrite.lastSent.sequenceNumber + 1 - sequenceNumber;
        _rewrite.offset.timestamp = projectedRtpTimestamp - header.timestamp.get();
    }
    else if (isPacketTooOld(sequenceNumber, MAX_REWIND_AUDIO))
    {
        return false;
    }

    const uint32_t newSequenceNumber = sequenceNumber + _rewrite.offset.sequenceNumber;
    _originalSsrc = currentSsrc;

    if (static_cast<int32_t>(newSequenceNumber - _rewrite.lastSent.sequenceNumber) > 0)
    {
        doRtpHeaderRewriteForAudio(header, senderInboundContext, newSequenceNumber, isTelephoneEvent);
        _rewrite.lastSent.sequenceNumber = newSequenceNumber;
        _rewrite.lastSent.timestamp = header.timestamp;
        _rewrite.lastSent.wallClock = timestamp;
        _rewrite.lastSent.lastOriginalSequenceNumber = sequenceNumber;
        return true;
    }

    const bool packetOlderThanLastDroppedTelephoneEvent =
        _rewrite.telephoneEvents.lastDroppedSequenceNumber > sequenceNumber;
    if (packetOlderThanLastDroppedTelephoneEvent)
    {
        // Here when the packet is older than last dropped telephone event,
        // although if the audio packet is newer than the last audio packet sent, we can still project the
        // sequence and re-adjust the _rewrite.offset.sequenceNumber. Otherwise packet must be dropped, as the there is
        // no good way to project the right sequence using the data we have;
        const bool canBeCalculated = _rewrite.lastSent.lastOriginalSequenceNumber < sequenceNumber;

        const int32_t projectedAdvance = (header.timestamp - _rewrite.lastSent.timestamp) /
            (codec::Opus::sampleRate / codec::Opus::packetsPerSecond);

        // If the projection is too high it is a small that something it might be wrong. Then if it's bigger than 60, we
        // will drop the packet. Projections <= 0 when `canBeCalculated` is `true` are definitely wrong
        if (canBeCalculated && projectedAdvance > 0 && projectedAdvance < 60)
        {
            const uint32_t correctedSeq = (_rewrite.lastSent.sequenceNumber + projectedAdvance);
            doRtpHeaderRewriteForAudio(header, senderInboundContext, correctedSeq, isTelephoneEvent);
            _rewrite.lastSent.lastOriginalSequenceNumber = sequenceNumber;
            _rewrite.lastSent.sequenceNumber = correctedSeq;
            _rewrite.lastSent.timestamp = header.timestamp;
            _rewrite.lastSent.wallClock = timestamp;
            _rewrite.offset.sequenceNumber += projectedAdvance;

            return true;
        }

        // With the data we stored, it is not possible to know what would be the right sequence for this late packet.
        return false;
    }

    // Late packet within safe limits
    doRtpHeaderRewriteForAudio(header, senderInboundContext, newSequenceNumber, isTelephoneEvent);
    return true;
}

bool SsrcOutboundContext::rewriteVideo(rtp::RtpHeader& header,
    const bridge::SsrcInboundContext& senderInboundContext,
    const uint32_t extendedSequenceNumber,
    const char* transportName,
    uint32_t& outExtendedSequenceNumber,
    const uint64_t timestamp,
    bool isKeyFrame)
{
    const bool isVp8 = rtpMap.format == bridge::RtpMap::Format::VP8;
    const uint32_t sampleRate = isVp8 ? codec::Vp8::sampleRate : 90000;

    uint8_t* rtpPayload = header.getPayload();
    const uint32_t rtpTimestamp = header.timestamp.get();
    const uint32_t originSsrc = header.ssrc.get();
    const uint16_t picId = isVp8 ? codec::Vp8Header::getPicId(rtpPayload) : 0;
    const uint8_t tl0PicIdx = isVp8 ? codec::Vp8Header::getTl0PicIdx(rtpPayload) : 0;
    const int32_t seqAdvance = static_cast<int32_t>(
        (extendedSequenceNumber + _rewrite.offset.sequenceNumber) - _rewrite.lastSent.sequenceNumber);

    if (_rewrite.empty())
    {
        _originalSsrc = originSsrc - 1; // to make it differ in next check
        _rewrite.lastSent.sequenceNumber = extendedSequenceNumber - 1;
        _rewrite.lastSent.picId = (picId - 1) & 0x7FFF;
        _rewrite.lastSent.tl0PicIdx = tl0PicIdx - 1;
        _rewrite.lastSent.timestamp = rtpTimestamp;
        _rewrite.lastSent.wallClock = timestamp;
        logger::info("rewriteVideo: %s start ssrc %u -> %u, sequence %u roc %u",
            "SsrcOutboundContext",
            transportName,
            originSsrc,
            ssrc,
            extractSequenceNumber(extendedSequenceNumber),
            extractRolloverCounter(extendedSequenceNumber));
    }

    if (_originalSsrc != originSsrc)
    {
        _originalSsrc = originSsrc;
        _rewrite.offset.sequenceNumber =
            math::ringDifference<uint32_t, 32>(extendedSequenceNumber, _rewrite.lastSent.sequenceNumber + 1);
        _rewrite.sequenceNumberStart = extendedSequenceNumber;

        const uint32_t projectedRtpTimestamp = _rewrite.lastSent.timestamp +
            utils::Time::diff(_rewrite.lastSent.wallClock, timestamp) * sampleRate / utils::Time::sec;
        _rewrite.offset.timestamp = math::ringDifference<uint32_t, 32>(rtpTimestamp, projectedRtpTimestamp);

        if (isVp8)
        {
            _rewrite.offset.picId = math::ringDifference<uint16_t, 15>(picId, _rewrite.lastSent.picId + 1);
            _rewrite.offset.tl0PicIdx = math::ringDifference<uint16_t, 8>(tl0PicIdx, _rewrite.lastSent.tl0PicIdx + 1);
        }

        REWRITER_LOG("rewriteVideo: %s new offset, ssrc %u, oseq %d, oPicId %d, otl0PicIdx %d, oTimestamp %d",
            "SsrcOutboundContext",
            transportName,
            originSsrc,
            ssrcRewrite.offset.sequenceNumber,
            ssrcRewrite.offset.picId,
            ssrcRewrite.offset.tl0PicIdx,
            ssrcRewrite.offset.timestamp);

        logger::info("rewriteVideo: %s ssrc %u -> %u, sequence %u, timestamp %u",
            "SsrcOutboundContext",
            transportName,
            originSsrc,
            ssrc,
            extendedSequenceNumber + _rewrite.offset.sequenceNumber,
            header.timestamp.get() + _rewrite.offset.timestamp);
    }
    else if (seqAdvance > MAX_VIDEO_SEQ_GAP)
    {
        _rewrite.offset.sequenceNumber =
            math::ringDifference<uint32_t, 32>(extendedSequenceNumber, _rewrite.lastSent.sequenceNumber + 1);
        const uint32_t projectedRtpTimestamp = _rewrite.lastSent.timestamp +
            utils::Time::diff(_rewrite.lastSent.wallClock, timestamp) * codec::Vp8::sampleRate / utils::Time::sec;
        _rewrite.offset.timestamp = math::ringDifference<uint32_t, 32>(rtpTimestamp, projectedRtpTimestamp);

        logger::debug("rewriteVideo: Major sequence number skip ssrc %u, seq %u, sent %u. Adjusting offset to hide it",
            "SsrcOutboundContext",
            header.ssrc.get(),
            extendedSequenceNumber,
            _rewrite.lastSent.sequenceNumber);
    }
    else if (isPacketTooOld(extendedSequenceNumber, MAX_REWIND_VIDEO))
    {
        return false;
    }

    outExtendedSequenceNumber = extendedSequenceNumber + _rewrite.offset.sequenceNumber;
    doRtpHeaderRewriteForVideo(header, senderInboundContext, outExtendedSequenceNumber);

    uint16_t newPicId = 0xFFFF;
    uint8_t newTl0PicIdx = 0xFF;
    if (isVp8)
    {
        newPicId = picId + _rewrite.offset.picId;
        newTl0PicIdx = tl0PicIdx + _rewrite.offset.tl0PicIdx;
        codec::Vp8Header::setPicId(rtpPayload, newPicId);
        codec::Vp8Header::setTl0PicIdx(rtpPayload, newTl0PicIdx);

        REWRITER_LOG(
            "rewriteVideo: (vp8) %s fwd ssrc %u -> %u, seq %u (%u) -> %u (%u), marker %u, picId %d -> %d, tl0PicIdx %d "
            "-> %d, ts %u -> %u",
            "RtpVideoRewriter",
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
            header.timestamp.get());
    }
    else
    {
        // We support vp8 and h264. So h264 if we are here
        REWRITER_LOG("rewriteVideo: %s fwd ssrc %u -> %u, seq %u (%u) -> %u (%u), marker %u, ts %u -> %u",
            "RtpVideoRewriter",
            transportName,
            ssrc,
            rtpHeader->ssrc.get(),
            extractSequenceNumber(extendedSequenceNumber),
            extractRolloverCounter(extendedSequenceNumber),
            extractSequenceNumber(outExtendedSequenceNumber),
            extractRolloverCounter(outExtendedSequenceNumber),
            rtpHeader->marker,
            timestamp,
            header.timestamp.get());
    }

    if (static_cast<int32_t>(outExtendedSequenceNumber - _rewrite.lastSent.sequenceNumber) > 0)
    {
        _rewrite.lastSent.sequenceNumber = outExtendedSequenceNumber;
        _rewrite.lastSent.timestamp = header.timestamp.get();
        _rewrite.lastSent.wallClock = timestamp;

        _rewrite.lastSent.picId = newPicId;
        _rewrite.lastSent.tl0PicIdx = newTl0PicIdx;
    }

    if (isKeyFrame)
    {
        this->lastKeyFrameSequenceNumber = outExtendedSequenceNumber;
    }

    return true;
}
