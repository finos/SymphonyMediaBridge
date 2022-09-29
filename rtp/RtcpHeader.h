#pragma once

#include "memory/Packet.h"
#include "utils/ByteOrder.h"
#include "utils/TlvIterator.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace rtp
{

enum RtcpPacketType : uint8_t
{
    SENDER_REPORT = 200,
    RECEIVER_REPORT = 201,
    SOURCE_DESCRIPTION = 202,
    GOODBYE = 203,
    APP_SPECIFIC = 204,
    RTPTRANSPORT_FB = 205,
    PAYLOADSPECIFIC_FB = 206
};

struct RtcpHeader
{
    uint8_t fmtCount : 5;
    uint8_t padding : 1;
    uint8_t version : 2;
    uint8_t packetType : 8;
    nwuint16_t length;

    static const uint32_t MAX_REPORT_BLOCKS;

    RtcpHeader() : fmtCount(0), padding(0), version(2), packetType(0), length(0) {}

    size_t size() const { return 4 + length * 4; }
    static RtcpHeader* fromPtr(void* p, size_t length);
    static const RtcpHeader* fromPtr(const void* p, size_t length) { return fromPtr(const_cast<void*>(p), length); }
    inline static RtcpHeader* fromPacket(memory::Packet& p) { return fromPtr(p.get(), p.getLength()); }
    size_t getPaddingSize() const;
    bool isValid() const;
    void addPadding(size_t wordCount);

    uint32_t getReporterSsrc() const { return reinterpret_cast<const nwuint32_t*>(this + 1)->get(); }
};

// Generic rtcp head that can be used on encrypted packets
struct RtcpReport
{
    RtcpHeader header;
    nwuint32_t ssrc;

    static const RtcpReport* fromPtr(const void* p, size_t length);
    inline static const RtcpReport* fromPacket(memory::Packet& p) { return fromPtr(p.get(), p.getLength()); }
};

// use to iterate through compound RTCP packet
class CompoundRtcpPacket
{
public:
    CompoundRtcpPacket(void* p, size_t length);
    CompoundRtcpPacket(const void* p, size_t length);
    typedef utils::TlvIterator<RtcpHeader> iterator;
    typedef utils::TlvIterator<const RtcpHeader> const_iterator;

    iterator begin() { return iterator(_first); }
    iterator end() { return iterator(_end); }
    const_iterator cbegin() const { return const_iterator(_first); }
    const_iterator cend() const { return const_iterator(_end); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    bool empty() const { return _first == _end; }

    static bool isValid(const void* p, size_t length);

private:
    RtcpHeader* const _first;
    RtcpHeader* _end;
};

struct SDESItem
{
    enum Type : uint8_t
    {
        CNAME = 1,
        NAME,
        EMAIL,
        PHONE,
        LOC,
        TOOL,
        NOTE
    };
    uint8_t type;
    uint8_t length;
    char data[255];

    SDESItem();
    explicit SDESItem(Type type);
    SDESItem(Type itemType, const std::string& value) : type(itemType), length(0) { setValue(value); }
    void setValue(const std::string& value);
    std::string getValue() const;
    size_t size() const { return type == 0 ? 1 : length + 2; }
    bool empty() const { return type == 0; } // also eof list
};

class RtcpSourceDescription
{
public:
    class Chunk
    {
    public:
        Chunk() : ssrc(0) {}
        explicit Chunk(uint32_t ssrc);
        Chunk(const Chunk&) = delete;
        size_t getCount() const;
        size_t size() const;
        void addItem(const SDESItem& item);
        bool isValid(const uint8_t* expectedEnd, size_t& chunkSize) const;

        typedef utils::TlvIterator<SDESItem> iterator;
        typedef utils::TlvIterator<const SDESItem> const_iterator;

        iterator begin() { return iterator(_items); }
        iterator end() { return iterator(const_cast<SDESItem*>(&*cend())); }
        const_iterator cbegin() const { return const_iterator(_items); }
        const_iterator cend() const;

        nwuint32_t ssrc;

    private:
        SDESItem _items[8]; // max 2048B
        // friend struct RtcpSourceDescription;
    };

    RtcpSourceDescription() { header.packetType = RtcpPacketType::SOURCE_DESCRIPTION; }
    RtcpSourceDescription(const RtcpSourceDescription&) = delete;
    static RtcpSourceDescription* create(void* buffer);
    size_t size() const { return header.size(); }
    void addChunk(const Chunk& chunk);
    int getChunkCount() const { return header.fmtCount; }

    bool isValid() const;

    typedef utils::TlvIterator<Chunk> iterator;
    typedef utils::TlvIterator<const Chunk> const_iterator;

    iterator begin() { return iterator(_chunks); }
    iterator end() { return iterator(const_cast<Chunk*>(&*cend())); }
    const_iterator cbegin() const { return const_iterator(_chunks); }
    const_iterator cend() const;
    RtcpHeader header;

private:
    Chunk _chunks[1]; // max 2048B
};

class LossCounter
{
public:
    LossCounter();
    uint32_t getCumulativeLoss() const;
    double getFractionLost() const;
    void setCumulativeLoss(uint32_t count);
    void setFractionLost(double fraction);

private:
    nwuint32_t _lossInfo;
};

class ReportBlock
{
public:
    nwuint32_t ssrc;
    LossCounter loss;
    nwuint32_t extendedSeqNoReceived;
    nwuint32_t interarrivalJitter;
    nwuint32_t lastSR;
    nwuint32_t delaySinceLastSR;

    void setDelaySinceLastSR(uint64_t ns);
};

struct RtcpReceiverReport
{
    RtcpHeader header;
    nwuint32_t ssrc;
    ReportBlock reportBlocks[31];

    RtcpReceiverReport();
    RtcpReceiverReport(const RtcpReceiverReport&) = delete;
    static RtcpReceiverReport* fromPtr(void* p, size_t length);
    static const RtcpReceiverReport* fromPtr(const void* p, size_t length)
    {
        return fromPtr(const_cast<void*>(p), length);
    }
    static RtcpReceiverReport* create(void* p);

    ReportBlock& addReportBlock(uint32_t ssrc);
    bool isValid() const;
};

class RtcpSenderReport
{
public:
    RtcpHeader header;
    nwuint32_t ssrc;
    nwuint32_t ntpSeconds;
    nwuint32_t ntpFractions;
    nwuint32_t rtpTimestamp;
    nwuint32_t packetCount;
    nwuint32_t octetCount;
    ReportBlock reportBlocks[31];

    RtcpSenderReport();
    RtcpSenderReport(const RtcpSenderReport&) = delete;
    static RtcpSenderReport* create(void* buffer);
    static RtcpSenderReport* fromPtr(void* buffer, size_t length);
    static const RtcpSenderReport* fromPtr(const void* buffer, size_t length)
    {
        return fromPtr(const_cast<void*>(buffer), length);
    }
    void setNtp(std::chrono::system_clock::time_point timestamp);
    uint64_t getNtp() const { return (static_cast<uint64_t>(ntpSeconds.get()) << 32) | ntpFractions.get(); }

    ReportBlock& addReportBlock(uint32_t ssrc);
    size_t size() const { return header.size(); }

    bool isValid() const;
};

class RtcpGoodbye
{
public:
    RtcpHeader header;
    nwuint32_t ssrc[31];

    RtcpGoodbye();
    static RtcpGoodbye* create(void* buffer);
    static RtcpGoodbye* create(void* buffer, uint32_t ssrc);
    void addSsrc(uint32_t ssrc);
};

class RtcpApplicationSpecific
{
public:
    RtcpHeader header;
    nwuint32_t ssrc;
    char name[4];

    RtcpApplicationSpecific();

    static RtcpApplicationSpecific* create(void* target, uint32_t ssrc, const char* name, uint16_t dataSize);
};

constexpr bool isRtcpPacket(const void* buffer, const uint32_t length)
{
    if (length < 8)
    {
        return false;
    }

    const auto header = reinterpret_cast<const RtcpHeader*>(buffer);
    return header->version == 2 && (header->packetType >= 192 && header->packetType < 224);
}

inline bool isRtcpPacket(const memory::Packet& packet)
{
    return isRtcpPacket(packet.get(), packet.getLength());
}

inline bool isValidRtcpPacket(const memory::Packet& packet)
{
    return CompoundRtcpPacket::isValid(packet.get(), packet.getLength());
}

} // namespace rtp
