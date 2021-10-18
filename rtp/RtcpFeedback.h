#pragma once

#include "rtp/RtcpHeader.h"
#include "utils/ByteOrder.h"
#include <cstddef>
#include <cstdint>

namespace rtp
{

struct RtcpFeedback
{
    RtcpHeader _header;
    nwuint32_t _reporterSsrc;
    nwuint32_t _mediaSsrc;
};

enum PayloadSpecificFeedbackType
{
    Pli = 1,
    Fir = 4,
    Application = 15
};

enum TransportLayerFeedbackType
{
    PacketNack = 1
};

RtcpFeedback* createPLI(void* buffer, const uint32_t fromSsrc, const uint32_t aboutSsrc);
size_t getNumFeedbackControlInfos(const RtcpFeedback* rtcpFeedback);

void getFeedbackControlInfo(const RtcpFeedback* rtcpFeedback,
    const size_t index,
    const size_t numFeedbackControlInfos,
    uint16_t& outPid,
    uint16_t& outBlp);

struct RtcpRembFeedback
{
    RtcpRembFeedback();
    uint64_t getBitRate() const;
    void setBitRate(uint64_t bps);
    static RtcpRembFeedback& create(void* area, uint32_t reporterSsrc);
    void addSsrc(uint32_t ssrc);
    uint32_t size() const;

    RtcpHeader header;
    nwuint32_t reporterSsrc;
    nwuint32_t mediaSsrc;
    char uniqueId[4]; // REMB
    uint8_t ssrcCount;

private:
    uint8_t _bitrate[3]; // 6 bit exp, 18 bit mantissa
public:
    nwuint32_t ssrcFeedback[]; // 255 items
};

bool isRemb(const void* header);
} // namespace rtp
