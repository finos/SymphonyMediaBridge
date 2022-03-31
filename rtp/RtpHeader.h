#pragma once

#include "memory/Packet.h"
#include "utils/ByteOrder.h"
#include "utils/TlvIterator.h"

namespace rtp
{

/**
 * Id of 15 means end of list and and iteration can be aborted
 */
class GeneralExtension1Byteheader
{
    uint8_t _length : 4;
    uint8_t _id : 4;

public:
    uint8_t data[20];

    GeneralExtension1Byteheader(uint8_t extensionId, uint8_t dataLength)
        : _length(std::max(uint8_t(1), dataLength) - 1),
          _id(extensionId),
          data{0}
    {
        data[0] = 0;
    }

    uint8_t getId() const { return _id; }
    void setDataLength(uint8_t length);
    uint8_t getDataLength() const;

    size_t size() const;
};

struct RtpHeaderExtension
{
    enum PROFILE
    {
        GENERAL1 = 0xbede,
        GENERAL2 = 0x100
    };
    nwuint16_t profile;
    nwuint16_t length;

    typedef utils::TlvIterator<GeneralExtension1Byteheader> iterator1;
    typedef utils::TlvIterator<const GeneralExtension1Byteheader> const_iterator1;

    RtpHeaderExtension() : profile(GENERAL1), length(0) { data[0] = 0; }
    explicit RtpHeaderExtension(const RtpHeaderExtension* extensions);

    utils::TlvCollectionConst<GeneralExtension1Byteheader> extensions() const;
    utils::TlvCollection<GeneralExtension1Byteheader> extensions();

    size_t size() const { return minSize() + length * sizeof(uint32_t); }
    constexpr static size_t minSize() { return 2 * sizeof(uint16_t); }
    void addExtension(iterator1& cursor, GeneralExtension1Byteheader& extension);
    bool empty() const { return length.get() == 0; }

    bool isValid() const;

private:
    uint8_t data[512];
};

const size_t MIN_RTP_HEADER_SIZE = 12;
// TODO some compilers may not honor the bit layout.
// In that case reading and shifting in the bits is needed.
struct RtpHeader
{
    uint16_t csrcCount : 4;
    uint16_t extension : 1;
    uint16_t padding : 1;
    uint16_t version : 2;
    uint16_t payloadType : 7;
    uint16_t marker : 1;
    nwuint16_t sequenceNumber;
    nwuint32_t timestamp;
    nwuint32_t ssrc;
    nwuint32_t csrc[15];

    static RtpHeader* fromPtr(void* p, size_t len);
    inline static const RtpHeader* fromPtr(const void* p, size_t len) { return fromPtr(const_cast<void*>(p), len); }
    template <typename PacketType>
    inline static RtpHeader* fromPacket(PacketType& p)
    {
        return fromPtr(p.get(), p.getLength());
    }
    template <typename PacketType>
    inline static const RtpHeader* fromPacket(const PacketType& p)
    {
        return fromPtr(p.get(), p.getLength());
    }
    static RtpHeader* create(void* p, size_t len);
    size_t headerLength() const;
    uint8_t* getPayload() { return reinterpret_cast<uint8_t*>(this) + headerLength(); }
    const uint8_t* getPayload() const { return const_cast<RtpHeader*>(this)->getPayload(); }

    RtpHeaderExtension* getExtensionHeader();
    const RtpHeaderExtension* getExtensionHeader() const { return const_cast<RtpHeader*>(this)->getExtensionHeader(); }
    void setExtensions(const RtpHeaderExtension& extensions);
};

constexpr bool isRtpPacket(const void* buffer, const uint32_t length)
{
    if (length < MIN_RTP_HEADER_SIZE)
    {
        return false;
    }

    const auto header = reinterpret_cast<const RtpHeader*>(buffer);
    return header->version == 2 && (header->payloadType < 64 || header->payloadType >= 96);
}

inline bool isRtpPacket(const memory::Packet& packet)
{
    return isRtpPacket(packet.get(), packet.getLength());
}

void setTransmissionTimestamp(memory::Packet* packet, uint8_t extensionId, uint64_t timestamp);
bool getTransmissionTimestamp(const memory::Packet& packet, uint8_t extensionId, uint32_t& sendTime);
} // namespace rtp
