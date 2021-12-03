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
    PacketNack = 1,
    TemporaryMaxMediaBitrate = 3
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
    uint64_t getBitrate() const;
    void setBitrate(uint64_t bps);
    static RtcpRembFeedback& create(void* area, uint32_t reporterSsrc);
    void addSsrc(uint32_t ssrc);

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

struct RtcpTemporaryMaxMediaBitrate
{
    struct Entry
    {
        uint64_t getBitrate() const;
        void setBitrate(uint64_t bps);
        uint32_t getPacketOverhead() const;
        void setPacketOverhead(uint32_t overhead);

        nwuint32_t ssrc;

    private:
        nwuint32_t _data;
    };

    RtcpTemporaryMaxMediaBitrate();

    static RtcpTemporaryMaxMediaBitrate& create(void* area, uint32_t reporterSsrc);

    uint32_t size() const { return header.size(); }
    uint32_t getCount() const { return (header.length - 2) / 2; }
    Entry& getEntry(size_t index);
    const Entry& getEntry(size_t index) const;
    Entry& addEntry(uint32_t ssrc, uint32_t bps, uint32_t packetOverhead);

    RtcpHeader header;
    nwuint32_t reporterSsrc;

private:
    nwuint32_t _mediaSsrc;
    Entry _entry[];
};

} // namespace rtp
