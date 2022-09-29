#include "RtcpHeader.h"
#include "utils/Time.h"
namespace rtp
{

const uint32_t RtcpHeader::MAX_REPORT_BLOCKS = 31;

RtcpHeader* RtcpHeader::fromPtr(void* p, const size_t length)
{
    assert((intptr_t)p % alignof(RtcpHeader) == 0);
    assert(length >= sizeof(RtcpHeader));
    if (length >= sizeof(RtcpHeader))
    {
        return reinterpret_cast<RtcpHeader*>(p);
    }

    return nullptr;
}

// padding byte count
size_t RtcpHeader::getPaddingSize() const
{
    if (!padding)
    {
        return 0;
    }
    const uint8_t* paddingCount =
        reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint32_t*>(this) + length + 1) - 1;
    assert(*paddingCount % 4 == 0);
    assert(*paddingCount > 4);
    return *paddingCount;
}

// can only add padding once.
// Number of int32 words to add as padding
// Max padding is 63 dwords, min padding is 1 dword
void RtcpHeader::addPadding(size_t wordCount)
{
    assert(wordCount < 256 / 4);
    assert(wordCount > 0);
    assert(!padding);

    memset(reinterpret_cast<uint8_t*>(this) + length * 4 + 4, 0, wordCount * 4);
    auto byteCount = reinterpret_cast<uint8_t*>(this) + length * 4 + wordCount * 4 + 3;
    *byteCount = wordCount * 4;
    padding = 1;
    length = length + wordCount;
}

bool RtcpHeader::isValid() const
{
    if (version != 2)
    {
        return false;
    }
    if (padding)
    {
        if (length > 0)
        {
            const auto paddingSize = getPaddingSize();
            return length * sizeof(uint32_t) >= paddingSize && ((paddingSize % 4) == 0);
        }
        return false;
    }
    return true;
}

const RtcpReport* RtcpReport::fromPtr(const void* p, size_t length)
{
    assert((intptr_t)p % alignof(RtcpReport) == 0);
    const auto* report = reinterpret_cast<const RtcpReport*>(p);
    if (length < sizeof(RtcpHeader) + sizeof(uint32_t) || report->header.size() > length || report->header.version != 2)
    {
        return nullptr;
    }
    return report;
}

void ReportBlock::setDelaySinceLastSR(uint64_t ns)
{
    constexpr uint64_t gcf = 512;
    constexpr uint64_t multiplier = 0x10000 / gcf;
    constexpr uint64_t divisor = 1000000000 / gcf;
    delaySinceLastSR = ns * multiplier / divisor;
}

RtcpReceiverReport::RtcpReceiverReport() : ssrc(0)
{
    header.length = 1;
    header.packetType = RECEIVER_REPORT;
}

RtcpReceiverReport* RtcpReceiverReport::fromPtr(void* p, size_t length)
{
    assert((intptr_t)p % alignof(RtcpReceiverReport) == 0);
    auto* r = reinterpret_cast<RtcpReceiverReport*>(p);
    if (length > sizeof(RtcpHeader) && r->header.size() <= length && r->header.packetType == RECEIVER_REPORT)
    {
        return r;
    }
    return nullptr;
}

RtcpReceiverReport* RtcpReceiverReport::create(void* p)
{
    auto report = reinterpret_cast<RtcpReceiverReport*>(p);
    std::memset(p, 0, sizeof(RtcpReceiverReport));
    report->header.version = 2;
    report->header.packetType = RECEIVER_REPORT;
    report->header.length = 1;
    return report;
}

bool RtcpReceiverReport::isValid() const
{
    return header.isValid() &&
        (sizeof(ssrc) + header.fmtCount * sizeof(ReportBlock) + header.getPaddingSize()) ==
        header.length * sizeof(uint32_t);
}

ReportBlock& RtcpReceiverReport::addReportBlock(uint32_t ssrc)
{
    assert(header.fmtCount < 31);
    reportBlocks[header.fmtCount++].ssrc = ssrc;
    uint16_t l = header.length;
    header.length = l + sizeof(ReportBlock) / sizeof(uint32_t);
    return reportBlocks[header.fmtCount - 1];
}

CompoundRtcpPacket::CompoundRtcpPacket(void* p, const size_t length)
    : _first(RtcpHeader::fromPtr(p, length)),
      _end(_first)
{
    assert((intptr_t)p % alignof(CompoundRtcpPacket) == 0);
    if (length < sizeof(RtcpHeader))
    {
        return;
    }

    const auto expectedEnd = reinterpret_cast<RtcpHeader*>(reinterpret_cast<uint8_t*>(p) + length);

    for (iterator it(_first); &(*it) <= expectedEnd; ++it)
    {
        _end = &(*it);
    }
}

CompoundRtcpPacket::CompoundRtcpPacket(const void* p, const size_t length)
    : _first(const_cast<RtcpHeader*>(RtcpHeader::fromPtr(p, length))),
      _end(_first)
{
    assert((intptr_t)p % alignof(CompoundRtcpPacket) == 0);
    if (length < sizeof(RtcpHeader))
    {
        return;
    }

    const auto expectedEnd = reinterpret_cast<const RtcpHeader*>(reinterpret_cast<const uint8_t*>(p) + length);

    for (const_iterator it(_first); &(*it) <= expectedEnd; ++it)
    {
        _end = const_cast<RtcpHeader*>(&(*it));
    }
}

// Validates the rtcp packets in the sequence that length specifiers add up and headers are valid
// Content of each rtcp packet is not validated
// Packets must be plain text
bool CompoundRtcpPacket::isValid(const void* p, size_t length)
{
    if (length < sizeof(RtcpHeader) || (length % sizeof(uint32_t) != 0))
    {
        return false;
    }

    auto* head = RtcpHeader::fromPtr(p, length);
    if (!head || head->version != 2)
    {
        return false;
    }

    bool padding = head->padding;
    const uint16_t firstPacketType = head->packetType;
    const auto expectedEnd = reinterpret_cast<const RtcpHeader*>(reinterpret_cast<const uint8_t*>(p) + length);
    for (head = reinterpret_cast<const RtcpHeader*>(reinterpret_cast<const uint32_t*>(head) + head->length + 1);
         head < expectedEnd && head->version == 2;
         head = reinterpret_cast<const RtcpHeader*>(reinterpret_cast<const uint32_t*>(head) + head->length + 1))
    {
        if (head->padding && !padding)
        {
            return false; // only allow padding on last rtcp packet
        }
        if (firstPacketType != SENDER_REPORT && firstPacketType != RECEIVER_REPORT)
        {
            return false;
        }
        padding = head->padding;
    }
    return head == expectedEnd;
}

SDESItem::SDESItem() : type(0), length(0)
{
    data[0] = data[1] = 0;
}

SDESItem::SDESItem(Type sdesType) : type(static_cast<uint8_t>(sdesType)), length(0)
{
    data[0] = data[1] = 0;
}

void SDESItem::setValue(const std::string& value)
{
    length = std::min(static_cast<size_t>(255), value.size());
    memcpy(data, value.c_str(), length);
}

std::string SDESItem::getValue() const
{
    return std::string(data, 0, length);
}

RtcpSourceDescription* RtcpSourceDescription::create(void* buffer)
{
    auto report = reinterpret_cast<RtcpSourceDescription*>(buffer);
    report->header.packetType = static_cast<uint16_t>(RtcpPacketType::SOURCE_DESCRIPTION);
    report->header.padding = 0;
    report->header.version = 2;
    report->header.fmtCount = 0;
    report->header.length = 0;

    return report;
}

void RtcpSourceDescription::addChunk(const Chunk& chunk)
{
    assert(header.fmtCount < 31);
    if (header.fmtCount == 31)
    {
        return;
    }

    iterator it = begin();
    for (int i = 0; i < header.fmtCount; ++i)
    {
        ++it;
    }
    const auto chunkSize = chunk.size();
    memcpy(&(*it), &chunk, chunkSize);
    header.fmtCount++;
    header.length = header.length + chunkSize / 4;
}

bool RtcpSourceDescription::isValid() const
{
    if (!header.isValid())
    {
        return false;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(_chunks);
    auto expectedEnd = p + header.length * 4 - header.getPaddingSize();

    int chunkCount = 0;
    while (p < expectedEnd)
    {
        size_t chunkSize = 0;
        auto chunk = reinterpret_cast<const Chunk*>(p);
        if (chunk->isValid(expectedEnd, chunkSize))
        {
            p += chunkSize;
            ++chunkCount;
        }
        else
        {
            return false;
        }
    }

    return chunkCount == header.fmtCount && p == expectedEnd;
}

// safe retrieval of cend
RtcpSourceDescription::const_iterator RtcpSourceDescription::cend() const
{
    auto* p = reinterpret_cast<const uint8_t*>(_chunks);
    auto expectedEnd = p + header.length * 4 - header.getPaddingSize();
    int count = 0;
    while (p < expectedEnd - sizeof(uint32_t))
    {
        ++count;
        size_t chunkSize = 0;
        if (!reinterpret_cast<const Chunk*>(p)->isValid(expectedEnd, chunkSize))
        {
            return cbegin();
        }
        p += chunkSize;
    }
    if (p == expectedEnd && count == header.fmtCount)
    {
        return const_iterator(reinterpret_cast<const Chunk*>(p));
    }
    return cbegin(); // do not iterate corrupt message
}

bool RtcpSourceDescription::Chunk::isValid(const uint8_t* expectedEnd, size_t& size) const
{
    auto it = cbegin();
    // an item takes at least 2 bytes, but an empty item is one byte
    for (; reinterpret_cast<const uint8_t*>(&*it) < expectedEnd - 2 && !it->empty(); ++it) {}

    if (reinterpret_cast<const uint8_t*>(&*it) < expectedEnd && it->empty())
    {
        size = (reinterpret_cast<const uint8_t*>(&*it) - reinterpret_cast<const uint8_t*>(&ssrc) + 4) & 0xFFFC;

        return static_cast<size_t>(expectedEnd - reinterpret_cast<const uint8_t*>(this)) >= size;
    }
    return false;
}

RtcpSourceDescription::Chunk::Chunk(uint32_t senderSsrc) : ssrc(senderSsrc)
{
    _items[0] = SDESItem(); // make sure type is 0
}

size_t RtcpSourceDescription::Chunk::getCount() const
{
    int i = 0;
    for (const_iterator it = cbegin(); !it->empty(); ++it)
    {
        ++i;
    }
    return i;
}

size_t RtcpSourceDescription::Chunk::size() const
{
    size_t size = sizeof(ssrc);
    for (auto it = cbegin(); !it->empty(); ++it)
    {
        size += it->size();
    }

    return (size + 4) & 0xFFFC; // aligned to 32 bit and always ending with one to four 0
}

void RtcpSourceDescription::Chunk::addItem(const SDESItem& item)
{
    auto size = 0;
    iterator it = begin();
    for (; !it->empty(); ++it)
    {
        size += it->size();
    }
    memcpy(&(*it), &item, item.size());
    size += item.size();
    ++it;
    std::memset(&(*it), 0, 4 - (size % 4) + 4); // also set type 0 for next item to mark EOL
}

// you must validate Chunk before using iterators
RtcpSourceDescription::Chunk::const_iterator RtcpSourceDescription::Chunk::cend() const
{
    auto it = cbegin();
    for (; !it->empty(); ++it) {}
    return it;
}

LossCounter::LossCounter() : _lossInfo(0) {}

uint32_t LossCounter::getCumulativeLoss() const
{
    const uint32_t lossInfo = _lossInfo;
    return lossInfo & 0x00FFFFFFu;
}

double LossCounter::getFractionLost() const
{
    const uint32_t lossInfo = _lossInfo;
    uint8_t fraction8 = lossInfo >> 24;

    return static_cast<double>(fraction8) / 256.0;
}

void LossCounter::setCumulativeLoss(uint32_t count)
{
    const uint32_t lossInfo = _lossInfo;
    _lossInfo = (count & 0x00FFFFFFu) | (lossInfo & 0xFF000000u);
}

void LossCounter::setFractionLost(double fraction)
{
    uint32_t lossInfo = _lossInfo;
    _lossInfo = (lossInfo & 0x00FFFFFFu) | (static_cast<uint32_t>(fraction * 256.0) << 24);
}

RtcpSenderReport::RtcpSenderReport() : ssrc(0)
{
    header.packetType = SENDER_REPORT;
    header.length = 6;
}

RtcpSenderReport* RtcpSenderReport::create(void* buffer)
{
    auto report = reinterpret_cast<RtcpSenderReport*>(buffer);
    std::memset(report, 0, sizeof(*report));
    report->header.version = 2;
    report->header.packetType = SENDER_REPORT;
    report->header.length = 6;
    return report;
}

RtcpSenderReport* RtcpSenderReport::fromPtr(void* buffer, size_t length)
{
    assert((intptr_t)buffer % alignof(RtcpSenderReport) == 0);
    auto* r = reinterpret_cast<RtcpSenderReport*>(buffer);
    if (length < sizeof(RtcpHeader) || r->size() > length || r->header.packetType != SENDER_REPORT)
    {
        return nullptr;
    }
    return r;
}

void RtcpSenderReport::setNtp(const std::chrono::system_clock::time_point timestamp)
{
    auto ntp = utils::Time::toNtp(timestamp);
    ntpSeconds = ntp >> 32;
    ntpFractions = ntp & 0xFFFFFFFFu;
}

ReportBlock& RtcpSenderReport::addReportBlock(uint32_t ssrc)
{
    assert(header.fmtCount < 31);
    reportBlocks[header.fmtCount++].ssrc = ssrc;
    uint16_t l = header.length;
    header.length = l + sizeof(ReportBlock) / sizeof(uint32_t);
    return reportBlocks[header.fmtCount - 1];
}

bool RtcpSenderReport::isValid() const
{
    return header.isValid() &&
        (6 * sizeof(uint32_t) + header.fmtCount * sizeof(ReportBlock) + header.getPaddingSize()) == header.length * 4;
}

RtcpGoodbye::RtcpGoodbye()
{
    header.packetType = RtcpPacketType::GOODBYE;
}

RtcpGoodbye* RtcpGoodbye::create(void* p)
{
    auto pkt = reinterpret_cast<RtcpGoodbye*>(p);
    std::memset(p, 0, sizeof(RtcpHeader));
    pkt->header.packetType = static_cast<uint8_t>(RtcpPacketType::GOODBYE);
    pkt->header.version = 2;
    pkt->header.length = 0;
    return pkt;
}

RtcpGoodbye* RtcpGoodbye::create(void* p, uint32_t ssrc)
{
    auto pkt = reinterpret_cast<RtcpGoodbye*>(p);
    std::memset(p, 0, sizeof(RtcpHeader));
    pkt->header.packetType = static_cast<uint8_t>(RtcpPacketType::GOODBYE);
    pkt->header.version = 2;
    pkt->header.length = 0;
    pkt->addSsrc(ssrc);
    return pkt;
}

void RtcpGoodbye::addSsrc(uint32_t ssrc)
{
    if (header.fmtCount == 31)
    {
        assert(false);
        return;
    }

    this->ssrc[header.fmtCount] = ssrc;
    header.fmtCount = header.fmtCount + 1;
    header.length = header.length + 1;
}

RtcpApplicationSpecific::RtcpApplicationSpecific()
{
    header.packetType = RtcpPacketType::APP_SPECIFIC;
    name[0] = '\0';
}

RtcpApplicationSpecific* RtcpApplicationSpecific::create(void* target,
    uint32_t ssrc,
    const char* name,
    uint16_t dataSize)
{
    dataSize = dataSize / sizeof(uint32_t);
    auto rtcp = reinterpret_cast<rtp::RtcpApplicationSpecific*>(target);
    rtcp->header = RtcpHeader();
    rtcp->header.packetType = RtcpPacketType::APP_SPECIFIC;
    rtcp->header.length = sizeof(RtcpApplicationSpecific) + dataSize - 1;

    std::memset(rtcp->name, 0, 4);
    std::strncpy(rtcp->name, name, 4);
    rtcp->ssrc = ssrc;
    std::memset(rtcp + 1, 0, dataSize * sizeof(uint32_t));

    return rtcp;
}

} // namespace rtp
