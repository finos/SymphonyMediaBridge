#include "rtp/RtcpFeedback.h"
#include <cassert>
#include <cmath>
namespace
{

size_t getFeedbackControlInfoSize(const rtp::RtcpFeedback* rtcpFeedback)
{
    return rtcpFeedback->_header.size() - sizeof(rtp::RtcpFeedback);
}

template <uint8_t mantissaBits, uint8_t exponentBits>
bool extractMantissaExponent(uint64_t value, uint32_t& mantissa, uint32_t& exponent)
{
    static_assert(mantissaBits <= 32, "mantissa too large");
    static_assert(exponentBits < 16, "exponent too large");

    exponent = 0;
    const uint64_t maxMantissa = (uint64_t(1) << mantissaBits) - 1;
    const uint32_t maxExponent = (1u << exponentBits) - 1;
    while (value & ~maxMantissa)
    {
        ++exponent;
        value >>= 1;
    }

    if (exponent > maxExponent)
    {
        return false;
    }
    mantissa = static_cast<uint32_t>(value);
    return true;
}
} // namespace

namespace rtp
{

RtcpFeedback* createPLI(void* buffer, const uint32_t fromSsrc, const uint32_t aboutSsrc)
{
    auto* feedback = reinterpret_cast<RtcpFeedback*>(buffer);
    feedback->_header.fmtCount = PayloadSpecificFeedbackType::Pli;
    feedback->_header.version = 2;
    feedback->_header.packetType = PAYLOADSPECIFIC_FB;
    feedback->_header.length = 2;
    feedback->_reporterSsrc = fromSsrc;
    feedback->_mediaSsrc = aboutSsrc;
    return feedback;
}

size_t getNumFeedbackControlInfos(const RtcpFeedback* rtcpFeedback)
{
    return getFeedbackControlInfoSize(rtcpFeedback) / 4;
}

void getFeedbackControlInfo(const RtcpFeedback* rtcpFeedback,
    const size_t index,
    const size_t numFeedbackControlInfos,
    uint16_t& outPid,
    uint16_t& outBlp)
{
    if (index >= numFeedbackControlInfos)
    {
        assert(false);
        return;
    }

    const auto feedbackControlInfo =
        reinterpret_cast<const uint8_t*>(rtcpFeedback) + sizeof(RtcpFeedback) + (index * 4);

    outPid = static_cast<uint16_t>(feedbackControlInfo[0]) << 8 | static_cast<uint16_t>(feedbackControlInfo[1]);
    outBlp = static_cast<uint16_t>(feedbackControlInfo[2]) << 8 | static_cast<uint16_t>(feedbackControlInfo[3]);
}

uint64_t RtcpRembFeedback::getBitrate() const
{
    uint32_t exponent = _bitrate[0] >> 2;
    uint64_t mantissa = (uint32_t(_bitrate[0]) & 0x3u) << 16 | uint32_t(_bitrate[1]) << 8 | _bitrate[2];
    return mantissa << exponent;
}

void RtcpRembFeedback::setBitrate(uint64_t bps)
{
    uint32_t exponent = 0;
    uint32_t mantissa = 0;
    extractMantissaExponent<18, 6>(bps, mantissa, exponent);

    _bitrate[0] = (exponent << 2) | (mantissa >> 16);
    _bitrate[1] = (mantissa >> 8) & 0xFFu;
    _bitrate[2] = mantissa & 0xFF;
}

RtcpRembFeedback::RtcpRembFeedback() : reporterSsrc(0), mediaSsrc(0), ssrcCount(0)
{
    header.length += 4;
    header.packetType = RtcpPacketType::PAYLOADSPECIFIC_FB;
    header.fmtCount = PayloadSpecificFeedbackType::Application;
    std::memcpy(uniqueId, "REMB", 4);
    std::memset(_bitrate, 0, 3);
}

RtcpRembFeedback& RtcpRembFeedback::create(void* area, uint32_t reporterSsrc)
{
    auto& remb = *new (area) RtcpRembFeedback();
    remb.reporterSsrc = reporterSsrc;

    return remb;
}

void RtcpRembFeedback::addSsrc(uint32_t ssrc)
{
    if (ssrcCount < 255)
    {
        ssrcFeedback[ssrcCount++] = ssrc;
        header.length += 1;
    }
}

bool isRemb(const void* p)
{
    auto& header = *reinterpret_cast<const RtcpHeader*>(p);
    if (header.packetType == RtcpPacketType::PAYLOADSPECIFIC_FB &&
        header.fmtCount == PayloadSpecificFeedbackType::Application && header.length >= 4)
    {
        auto& remb = reinterpret_cast<const RtcpRembFeedback&>(header);
        return std::memcmp(remb.uniqueId, "REMB", 4) == 0;
    }
    return false;
}

RtcpTemporaryMaxMediaBitrate::RtcpTemporaryMaxMediaBitrate() : reporterSsrc(0), _mediaSsrc(0)
{
    header.length = 2;
    header.packetType = RtcpPacketType::RTPTRANSPORT_FB;
    header.fmtCount = TransportLayerFeedbackType::TemporaryMaxMediaBitrate;
}

RtcpTemporaryMaxMediaBitrate& RtcpTemporaryMaxMediaBitrate::create(void* area, uint32_t reporterSsrc)
{
    auto& tmmbr = *new (area) RtcpTemporaryMaxMediaBitrate();
    tmmbr.reporterSsrc = reporterSsrc;
    return tmmbr;
}

uint64_t RtcpTemporaryMaxMediaBitrate::Entry::getBitrate() const
{
    const uint32_t value = _data;
    const uint32_t exponent = value >> 26;
    const uint64_t mantissa = (value >> 9) & 0x1FFFFu;
    return mantissa << exponent;
}

void RtcpTemporaryMaxMediaBitrate::Entry::setBitrate(uint64_t bps)
{
    uint32_t exponent = 0; // 6 bit
    uint32_t mantissa = 0;
    extractMantissaExponent<17, 6>(bps, mantissa, exponent);
    const uint32_t overhead = _data.get() & 0x1FFu; // 9 bit

    _data = (exponent << 26) | (mantissa << 9) | overhead;
}

uint32_t RtcpTemporaryMaxMediaBitrate::Entry::getPacketOverhead() const
{
    return _data.get() & 0x1FFu;
}

void RtcpTemporaryMaxMediaBitrate::Entry::setPacketOverhead(uint32_t overhead)
{
    const uint32_t value = _data;
    _data = (value & ~0x1FFu) | (overhead & 0x1FFu);
}

RtcpTemporaryMaxMediaBitrate::Entry& RtcpTemporaryMaxMediaBitrate::getEntry(size_t index)
{
    return _entry[index];
}

const RtcpTemporaryMaxMediaBitrate::Entry& RtcpTemporaryMaxMediaBitrate::getEntry(size_t index) const
{
    return _entry[index];
}

RtcpTemporaryMaxMediaBitrate::Entry& RtcpTemporaryMaxMediaBitrate::addEntry(uint32_t ssrc,
    uint32_t bps,
    uint32_t packetOverhead)
{
    auto& entry = _entry[getCount()];
    header.length += 2;
    entry.ssrc = ssrc;
    entry.setBitrate(bps);
    entry.setPacketOverhead(packetOverhead);
    return entry;
}

} // namespace rtp
