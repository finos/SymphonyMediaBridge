#include "rtp/RtcpFeedback.h"
#include <cassert>
#include <cmath>
namespace
{

size_t getFeedbackControlInfoSize(const rtp::RtcpFeedback* rtcpFeedback)
{
    return rtcpFeedback->_header.size() - sizeof(rtp::RtcpFeedback);
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

uint64_t RtcpRembFeedback::getBitRate() const
{
    uint32_t exponent = _bitrate[0] >> 2;
    uint64_t mantissa = (uint32_t(_bitrate[0]) & 0x3u) << 16 | uint32_t(_bitrate[1]) << 8 | _bitrate[2];
    return mantissa << exponent;
}

void RtcpRembFeedback::setBitRate(uint64_t bps)
{
    uint8_t exponent = 0;
    const uint64_t maxMantissa = (1 << 18) - 1;
    while (bps & ~maxMantissa)
    {
        ++exponent;
        bps >>= 1;
    }
    _bitrate[0] = (exponent << 2) | (bps >> 16);
    _bitrate[1] = (bps >> 8) & 0xFFu;
    _bitrate[2] = bps & 0xFF;
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

} // namespace rtp
