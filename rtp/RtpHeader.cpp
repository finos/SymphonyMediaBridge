#include "rtp/RtpHeader.h"
#include "logger/Logger.h"
#include "utils/StringBuilder.h"
#include "utils/Time.h"
#include <array>

namespace
{
void logPacketError(void* p, const size_t len, const size_t baseHeaderLength)
{
    logger::debug("RTP packet header invalid. Length %lu, baseHeaderLength %lu", "RtpHeader", len, baseHeaderLength);
    const auto pBytes = reinterpret_cast<uint8_t*>(p);
    std::array<char, 4> byteString;
    utils::StringBuilder<256> loggerString;
    for (size_t i = 0; i < rtp::MIN_RTP_HEADER_SIZE; ++i)
    {
        snprintf(byteString.data(), byteString.size(), "%02x ", pBytes[i]);
        loggerString.append(byteString.data());
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

void RtpHeader::setExtensions(const RtpHeaderExtension& extensions, size_t payloadLength)
{
    auto currentExtensions = getExtensionHeader();
    ssize_t diff = 0;
    if (currentExtensions)
    {
        diff = static_cast<ssize_t>(extensions.size() - currentExtensions->size());
    }
    else
    {
        diff = extensions.size();
    }

    std::memmove(getPayload() + diff, getPayload(), payloadLength);

    extension = 0;
    const auto rtpBaseHeaderLength = headerLength();
    auto target = reinterpret_cast<char*>(this) + rtpBaseHeaderLength;
    extension = 1;
    std::memcpy(target, &extensions, extensions.size());
}

void RtpHeaderExtension::addExtension(iterator1& cursor, const GeneralExtension1Byteheader& extension)
{
    if (extension.getId() == ExtHeaderIdentifiers::PADDING)
    {
        return; // padding is automatically added
    }

    const auto target = reinterpret_cast<uint8_t*>(&(*cursor));
    const int newLength = target + extension.size() - data;
    const int diff = newLength % sizeof(uint32_t);
    const int padding = (diff ? sizeof(uint32_t) - diff : 0);

    std::memcpy(target, &extension, extension.size() + padding);
    length = (newLength + padding) / sizeof(uint32_t);

    ++cursor;
}

namespace
{
const uint8_t* findExtensionsEnd(const uint8_t* data, const uint8_t* dataEnd)
{
    const uint8_t* endCandidate = nullptr;
    for (const uint8_t* cursor = data; cursor < dataEnd;)
    {
        const auto& item = *reinterpret_cast<const GeneralExtension1Byteheader*>(cursor);
        if (item.getId() == ExtHeaderIdentifiers::PADDING && !endCandidate)
        {
            endCandidate = cursor;
        }
        if (item.getId() == ExtHeaderIdentifiers::EOL)
        {
            if (endCandidate)
            {
                return endCandidate;
            }
            return cursor;
        }
        else if (cursor + item.size() > dataEnd)
        {
            // corrupt
            return cursor;
        }
        else
        {
            cursor += item.size();
            if (item.getId() != ExtHeaderIdentifiers::PADDING)
            {
                endCandidate = nullptr;
            }
        }
    }
    if (endCandidate)
    {
        return endCandidate;
    }
    return dataEnd;
}
} // namespace

utils::TlvCollectionConst<GeneralExtension1Byteheader> RtpHeaderExtension::extensions() const
{
    if (profile.get() != GENERAL1)
    {
        return utils::TlvCollectionConst<GeneralExtension1Byteheader>(data, data);
    }

    return utils::TlvCollectionConst<GeneralExtension1Byteheader>(data,
        findExtensionsEnd(data, data + length * sizeof(uint32_t)));
}

utils::TlvCollection<GeneralExtension1Byteheader> RtpHeaderExtension::extensions()
{
    if (profile.get() != GENERAL1)
    {
        return utils::TlvCollection<GeneralExtension1Byteheader>(data, data);
    }

    return utils::TlvCollection<GeneralExtension1Byteheader>(data,
        const_cast<uint8_t*>(findExtensionsEnd(data, data + length * sizeof(uint32_t))));
}

bool RtpHeaderExtension::isValid() const
{
    const uint8_t* dataEnd = data + length * sizeof(uint32_t);
    for (const uint8_t* cursor = data; cursor < dataEnd;)
    {
        const auto& item = *reinterpret_cast<const GeneralExtension1Byteheader*>(cursor);
        if (item.getId() == ExtHeaderIdentifiers::EOL)
        {
            return true;
        }
        else if (cursor + item.size() > dataEnd)
        {
            // corrupt
            return false;
        }
        else
        {
            cursor += item.size();
        }
    }

    return true;
}

size_t GeneralExtension1Byteheader::size() const
{
    if (_id == ExtHeaderIdentifiers::PADDING)
    {
        return 1;
    }
    return 2 + _length;
}

void GeneralExtension1Byteheader::setDataLength(uint8_t length)
{
    _length = std::max(uint8_t(1), length) - 1;
}

uint8_t GeneralExtension1Byteheader::getDataLength() const
{
    return _length + 1;
}

void GeneralExtension1Byteheader::fillWithPadding()
{
    std::memset(reinterpret_cast<uint8_t*>(this), 0, size());
}

// convert to 24 bit seconds fixed point format 6.18
uint32_t nsToSecondsFp6_18(uint64_t timestampNs)
{
    const uint64_t GCD = 512; // to minimize shift out
    const uint64_t NOMINATOR = (1 << 18) / GCD;
    const uint64_t DENOMINATOR = utils::Time::sec / GCD;
    return ((timestampNs * NOMINATOR) / DENOMINATOR) & 0xFFFFFFu;
}

void setTransmissionTimestamp(memory::Packet& packet, uint8_t extensionId, uint64_t timestamp)
{
    if (!rtp::isRtpPacket(packet))
    {
        return;
    }

    auto* rtpHeader = RtpHeader::fromPacket(packet);
    if (!rtpHeader)
    {
        return;
    }

    auto* extensionHeader = rtpHeader->getExtensionHeader();
    if (extensionHeader)
    {
        for (auto& extension : extensionHeader->extensions())
        {
            if (extension.getId() == extensionId && extension.getDataLength() == 3)
            {
                auto ntpTimestamp = nsToSecondsFp6_18(timestamp);
                extension.data[0] = ntpTimestamp >> 16;
                extension.data[1] = (ntpTimestamp >> 8) & 0xFFu;
                extension.data[2] = ntpTimestamp & 0xFFu;
                return;
            }
        }
    }

    assert(false); // this will stop in debug for mobiles where not abs send time is set
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
            if (extension.getId() == extensionId)
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

bool getAudioLevel(const memory::Packet& packet, uint8_t extensionId, int& level)
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
            if (extension.getId() == extensionId)
            {
                level = extension.data[0] & 0x7F;
                return true;
            }
        }
    }

    return false;
}

} // namespace rtp
