#include "rtp/RtpHeader.h"
#include "logger/Logger.h"
#include "utils/StringBuilder.h"
#include "utils/Time.h"

namespace
{
void logPacketError(void* p, const size_t len, const size_t baseHeaderLength)
{
    logger::debug("RTP packet header invalid. Length %lu, baseHeaderLength %lu", "RtpHeader", len, baseHeaderLength);
    const auto pBytes = reinterpret_cast<uint8_t*>(p);
    char byteString[4];
    utils::StringBuilder<256> loggerString;
    for (size_t i = 0; i < rtp::MIN_RTP_HEADER_SIZE; ++i)
    {
        sprintf(byteString, "%02x ", pBytes[i]);
        loggerString.append(byteString);
    }

    logger::debug("%s", "RtpHeader", loggerString.get());
}
} // namespace

namespace rtp
{

RtpHeader* RtpHeader::fromPtr(void* p, const size_t len)
{
    assert((intptr_t)p % alignof(RtpHeader) == 0);
    auto header = reinterpret_cast<RtpHeader*>(p);
    assert(len >= MIN_RTP_HEADER_SIZE);
    const size_t baseHeaderLength = MIN_RTP_HEADER_SIZE + header->csrcCount * sizeof(u_int32_t);
    if (!(header->payloadType < 64 || header->payloadType >= 96) || len < baseHeaderLength ||
        (header->extension && len < baseHeaderLength + RtpHeaderExtension::minSize()))
    {
        logPacketError(p, len, baseHeaderLength);
        return nullptr;
    }

    if (header->extension)
    {
        auto rtpHeaderExtension =
            reinterpret_cast<const RtpHeaderExtension*>(reinterpret_cast<const char*>(p) + baseHeaderLength);
        if (len < rtpHeaderExtension->size() + baseHeaderLength)
        {
            logPacketError(p, len, baseHeaderLength);
            return nullptr;
        }
    }
    return header;
}

RtpHeader* RtpHeader::create(void* p, const size_t len)
{
    assert(len >= MIN_RTP_HEADER_SIZE);
    auto header = reinterpret_cast<RtpHeader*>(p);
    std::memset(header, 0, MIN_RTP_HEADER_SIZE);
    header->version = 2;
    return header;
}

size_t RtpHeader::headerLength() const
{
    const size_t baseHeaderLength = MIN_RTP_HEADER_SIZE + csrcCount * sizeof(u_int32_t);
    if (extension)
    {
        auto rtpHeaderExtension =
            reinterpret_cast<const RtpHeaderExtension*>(reinterpret_cast<const char*>(this) + baseHeaderLength);
        return baseHeaderLength + rtpHeaderExtension->size();
    }
    return baseHeaderLength;
}

RtpHeaderExtension::RtpHeaderExtension(const RtpHeaderExtension* extensions) : profile(GENERAL1), length(0)
{
    if (extensions)
    {
        std::memcpy(this, extensions, extensions->size());
    }
}

RtpHeaderExtension* RtpHeader::getExtensionHeader()
{
    const size_t baseHeaderLength = MIN_RTP_HEADER_SIZE + csrcCount * sizeof(u_int32_t);
    if (extension)
    {
        return reinterpret_cast<RtpHeaderExtension*>(reinterpret_cast<char*>(this) + baseHeaderLength);
    }
    return nullptr;
}

// notice that no csrc can be added after this
// you must add payload after this
void RtpHeader::setExtensions(const RtpHeaderExtension& extensions)
{
    const size_t baseHeaderLength = MIN_RTP_HEADER_SIZE + csrcCount * sizeof(u_int32_t);
    extension = 1;
    auto target = reinterpret_cast<char*>(this) + baseHeaderLength;
    std::memcpy(target, &extensions, extensions.size());
}

void RtpHeaderExtension::addExtension(iterator1& cursor, GeneralExtension1Byteheader& extension)
{
    std::memcpy(&(*cursor), &extension, extension.size());
    ++cursor;
    const int newLength = reinterpret_cast<uint8_t*>(&(*cursor)) - reinterpret_cast<uint8_t*>(&(*extensions().begin()));
    const int diff = newLength % sizeof(uint32_t);
    if (diff == 0)
    {
        length = newLength / sizeof(uint32_t);
    }
    else
    {
        const int padding = sizeof(uint32_t) - diff;
        for (int i = 0; i < padding; ++i)
        {
            *cursor = GeneralExtension1Byteheader();
            ++cursor;
        }
        length = (newLength + padding) / sizeof(uint32_t);
    }
}

size_t GeneralExtension1Byteheader::size() const
{
    if (id == 0 && len == 0)
    {
        return 1; // 0 padding
    }
    return 2 + len;
}

void GeneralExtension1Byteheader::setDataLength(int length)
{
    if (length < 2)
    {
        len = 0;
    }
    else
    {
        len = length - 1;
    }
}

// convert to 24 bit seconds fixed point format 6.18
uint32_t nsToSecondsFp6_18(uint64_t timestampNs)
{
    const uint64_t GCD = 512; // to minimize shift out
    const uint64_t NOMINATOR = (1 << 18) / GCD;
    const uint64_t DENOMINATOR = utils::Time::sec / GCD;
    return ((timestampNs * NOMINATOR) / DENOMINATOR) & 0xFFFFFFu;
}

void setTransmissionTimestamp(memory::Packet* packet, uint8_t extensionId, uint64_t timestamp)
{
    if (!rtp::isRtpPacket(*packet))
    {
        return;
    }

    auto* rtpHeader = RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        return;
    }

    auto* extensionHeader = rtpHeader->getExtensionHeader();
    if (extensionHeader)
    {
        for (auto& extension : extensionHeader->extensions())
        {
            if (extension.id == extensionId)
            {
                auto ntpTimestamp = nsToSecondsFp6_18(timestamp);
                extension.data[0] = ntpTimestamp >> 16;
                extension.data[1] = (ntpTimestamp >> 8) & 0xFFu;
                extension.data[2] = ntpTimestamp & 0xFFu;
            }

            return;
        }
    }

    assert(false);
    // we could insert header but rely on timestamp always being negotiated
    // and set in the forwarded packet.
}

bool getTransmissionTimestamp(const memory::Packet& packet, uint8_t extensionId, uint32_t& sendTime)
{
    assert(rtp::isRtpPacket(packet));

    auto* rtpHeader = RtpHeader::fromPacket(packet);
    if (!rtpHeader)
    {
        return false;
    }

    auto* extensionHeader = rtpHeader->getExtensionHeader();
    if (extensionHeader)
    {
        for (auto& extension : extensionHeader->extensions())
        {
            if (extension.id == extensionId)
            {
                sendTime = extension.data[0];
                sendTime <<= 8;
                sendTime += extension.data[1];
                sendTime <<= 8;
                sendTime += extension.data[2];
                return true;
            }
        }
    }

    return false;
}

} // namespace rtp
