#include "Sctprotocol.h"
#include "crypto/SslHelper.h"

namespace sctp
{

bool isCookieEcho(const void* data, size_t length)
{
    SctpPacket packet(data, length);
    if (!packet.isValid())
    {
        return false;
    }
    return packet.hasChunk(ChunkType::COOKIE_ECHO);
}

bool isSctpInit(const void* data, size_t length)
{
    SctpPacket packet(data, length);
    if (!packet.isValid())
    {
        return false;
    }
    if (packet.getHeader().verificationTag != 0)
    {
        return false;
    }
    return packet.hasChunk(ChunkType::INIT);
}

uint32_t getAssociationTag(const void* data, size_t length)
{
    SctpPacket packet(data, length);
    if (!packet.isValid())
    {
        return 0;
    }
    return packet.getHeader().verificationTag;
}

uint16_t getDestinationPort(const void* data, size_t length)
{
    SctpPacket packet(data, length);
    if (!packet.isValid())
    {
        return 0;
    }
    return packet.getHeader().destinationPort;
}

size_t ChunkParameter::size() const
{
    const int residual = length % 4;
    return length + (residual == 0 ? 0 : 4 - residual);
}

const size_t ChunkParameter::dataSize() const
{
    return length - HEADER_SIZE;
}

const uint8_t* ChunkParameter::data() const
{
    return reinterpret_cast<const uint8_t*>(&length + 1);
}

uint8_t* ChunkParameter::data()
{
    return reinterpret_cast<uint8_t*>(&length + 1);
}

void ChunkParameter::zeroPad()
{
    const size_t tail = size() - length;

    for (size_t i = 0; i < tail; ++i)
    {
        data()[dataSize() + i] = 0;
    }
}

utils::TlvCollection<ChunkParameter> ChunkField::params()
{
    return utils::TlvCollection<ChunkParameter>(&header + 1, reinterpret_cast<uint8_t*>(&header) + size());
}

utils::TlvCollectionConst<ChunkParameter> ChunkField::params() const
{
    return utils::TlvCollectionConst<ChunkParameter>(&header + 1, reinterpret_cast<const uint8_t*>(&header) + size());
}

// you must check that this fits in the data area prepared for the Chunk
void ChunkField::add(const ChunkParameter& parameter)
{
    auto target = &*params().end();
    std::memcpy(target, &parameter, parameter.size());
    target->zeroPad();
    header.length = header.length + parameter.size();
}

size_t ChunkField::size() const
{
    const int residual = header.length % 4;
    return header.length + (residual == 0 ? 0 : 4 - residual);
}

void ChunkField::commitAppendedParameter()
{
    auto& param = *params().end();
    param.zeroPad();
    header.length = header.length + param.size();
}

CookieEchoChunk::CookieEchoChunk() : ChunkField(ChunkType::COOKIE_ECHO)
{
    header.length = ChunkField::BASE_HEADER_SIZE;
}

void CookieEchoChunk::setCookie(const ChunkParameter& param)
{
    setCookie(param.data(), param.dataSize());
}

void CookieEchoChunk::setCookie(const void* cookie, size_t length)
{
    std::memcpy(&header + 1, cookie, length);
    header.length = header.length + length;
}

void SupportedExtensionsParameter::add(ChunkType chunkType)
{
    data()[getCount()] = chunkType;
    length = length + 1;
}

SctpPacket::SctpPacket(const void* packet, size_t size)
    : _packetSize(size),
      _commonHeader(const_cast<CommonHeader*>(reinterpret_cast<const CommonHeader*>(packet))){};

utils::TlvCollectionConst<ChunkField> SctpPacket::chunks() const
{
    return utils::TlvCollectionConst<ChunkField>(_commonHeader + 1,
        reinterpret_cast<const uint8_t*>(_commonHeader) + size());
}

// provide 4 bytes extra in data area becuase we will add a guard at the end
SctpPacketW::SctpPacketW(uint32_t tag, uint16_t srcPort, uint16_t dstPort, uint8_t* dataArea, size_t areaSize)
    : SctpPacket(dataArea, sizeof(CommonHeader)),
      _areaSize(areaSize - sizeof(uint32_t))
{
    _defaultArea[0] = 0;
    _commonHeader->sourcePort = srcPort;
    _commonHeader->destinationPort = dstPort;
    _commonHeader->verificationTag = tag;
    auto& guard = *reinterpret_cast<uint32_t*>(dataArea + _areaSize);
    guard = 0xCDCDCDCD;
}

SctpPacketW::SctpPacketW(uint32_t tag, uint16_t srcPort, uint16_t dstPort)
    : SctpPacket(_defaultArea, sizeof(CommonHeader)),
      _areaSize(sizeof(_defaultArea) - sizeof(uint32_t))
{
    _defaultArea[0] = 0;
    _commonHeader->sourcePort = srcPort;
    _commonHeader->destinationPort = dstPort;
    _commonHeader->verificationTag = tag;
    auto& guard = *reinterpret_cast<uint32_t*>(_defaultArea + _areaSize);
    guard = 0xCDCDCDCD;
}

void SctpPacketW::checkGuard() const
{
#ifdef DEBUG
    const auto guard = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(_commonHeader) + _areaSize);
    assert(guard == 0xCDCDCDCD);
#endif
}

void SctpPacketW::add(const ChunkField& field)
{
    assert(_packetSize + field.size() <= _areaSize);
    auto target = reinterpret_cast<uint8_t*>(&*chunks().end());
    std::memcpy(target, &field, field.size());
    assert(field.size() % 4 == 0);
    _packetSize += field.size();
    checkGuard();
}

size_t SctpPacketW::capacity() const
{
    return _areaSize - _packetSize;
}

utils::TlvCollection<ChunkField> SctpPacketW::chunks()
{
    return utils::TlvCollection<ChunkField>(_commonHeader + 1, reinterpret_cast<uint8_t*>(_commonHeader) + size());
}
// usage:
// SctpPacketW pkt;
// assert(pkt.capacity() >= myChunk.size());
// std::memcpy(pkt.end(), myChunk, myChunk.size());
// pkt.commitAppendedChunk();
void SctpPacketW::commitAppendedChunk()
{
    checkGuard();
    const auto& chunk = *chunks().end();
    _packetSize += chunk.size();
    assert(_packetSize <= _areaSize);
}

void SctpPacketW::clear()
{
    _packetSize = sizeof(CommonHeader);
}

namespace
{
crypto::Crc32Polynomial sctpPolynomial(0x1EDC6F41u);
}

uint32_t SctpPacket::calculateCheckSum() const
{
    crypto::Crc32 crc(sctpPolynomial);

    crc.add(_commonHeader, sizeof(CommonHeader) - sizeof(uint32_t));
    const uint32_t zeroChecksum = 0;
    crc.add(zeroChecksum);
    crc.add(_commonHeader + 1, _packetSize - sizeof(CommonHeader));
    return crc.compute();
}

// TODO check also that chunks fit total packet size
// Note: verificationTag si not allowed to be zero unless for INIT message
bool SctpPacket::isValid() const
{
    if (_packetSize < sizeof(CommonHeader) || (_packetSize % sizeof(uint32_t)) != 0)
    {
        return false;
    }

    const auto& header = getHeader();
    const uint32_t calculatedChecksum = calculateCheckSum();

    if (header.checksum != calculatedChecksum)
    {
        return false;
    }
    size_t s = sizeof(header);
    int index = 0;
    for (auto& chunk : chunks())
    {
        if (s + chunk.size() > _packetSize)
        {
            return false;
        }
        if (chunk.header.type == ChunkType::INIT && header.verificationTag == 0 && index == 0)
        {
            return true;
        }
        else if (header.verificationTag == 0)
        {
            return false;
        }

        s += chunk.size();
        ++index;
    }
    return true;
}

PayloadDataChunk::PayloadDataChunk()
    : ChunkField(ChunkType::DATA),
      transmissionSequenceNumber(0),
      streamId(0),
      streamSequenceNumber(0),
      payloadProtocol(0)
{
    header.length = HEADER_SIZE;
    _data[0] = 0;
}

PayloadDataChunk::PayloadDataChunk(uint16_t streamId_,
    uint16_t sequenceNumber_,
    uint32_t payloadProtocol_,
    uint32_t tsn_)
    : ChunkField(ChunkType::DATA),
      transmissionSequenceNumber(tsn_),
      streamId(streamId_),
      streamSequenceNumber(sequenceNumber_),
      payloadProtocol(payloadProtocol_)
{
    header.length = HEADER_SIZE;
    _data[0] = 0;
}

void PayloadDataChunk::clear()
{
    header.type = ChunkType::DATA;
    header.length = HEADER_SIZE;
    header.flags = 0;
    transmissionSequenceNumber = 0;
    streamId = 0;
    streamSequenceNumber = 0;
    payloadProtocol = 0;
}

void PayloadDataChunk::writeData(const void* data, size_t length, bool fragmentBegin, bool fragmentEnd)
{
    header.length = HEADER_SIZE + length;
    std::memcpy(_data, data, length);
    if (fragmentBegin)
    {
        setFragmentBegin();
    }
    if (fragmentEnd)
    {
        setFragmentEnd();
    }
}

CauseCode::CauseCode(ErrorCause cause, const char* reason) : ChunkParameter(static_cast<ChunkParameterType>(cause))
{
    auto endPtr = std::strncpy(reinterpret_cast<char*>(&length + 1), reason, sizeof(_data) - 1);
    auto reasonLength = endPtr - reason;

    length = length.get() + reasonLength;
    zeroPad();
}

CauseCode::CauseCode(ErrorCause cause, const uint32_t value) : ChunkParameter(static_cast<ChunkParameterType>(cause))
{
    std::memcpy(_data, &value, sizeof(value));
    length = HEADER_SIZE + sizeof(value);
}

std::string CauseCode::getReason() const
{
    auto* data = reinterpret_cast<const char*>(&length + 1);
    return std::string(data, length - 4);
}

void CauseCode::setData(const uint32_t value)
{
    std::memcpy(_data, &value, sizeof(value));
    length = HEADER_SIZE + sizeof(value);
}

AckRange SelectiveAckChunk::getAck(int index) const
{
    AckRange block;
    auto& rawBlock = reinterpret_cast<const RawGapAckBlock&>(data()[index]);
    block.start = rawBlock.start + cumulativeTsnAck;
    block.end = rawBlock.end + cumulativeTsnAck;
    return block;
}

uint32_t SelectiveAckChunk::getDuplicate(int index) const
{
    return data()[gapAckBlockCount + index];
}

SackBuilder::SackBuilder(uint8_t* data, size_t areaSize)
    : ack(reinterpret_cast<SelectiveAckChunk&>(*data)),
      _areaSize(areaSize)
{
    new (data) SelectiveAckChunk();
}

SackBuilder::SackBuilder(SelectiveAckChunk* ack_, size_t areaSize) : ack(*ack_), _areaSize(areaSize) {}

// Make sure you have set the cumulativeAck before calling addAck
void SackBuilder::addAck(uint32_t startTsn, uint32_t endTsn)
{
    auto blocks = reinterpret_cast<SelectiveAckChunk::RawGapAckBlock*>(ack.data());
    auto& block = blocks[ack.gapAckBlockCount];
    if (reinterpret_cast<uint32_t*>(&block + 1) > ack.data() + _areaSize / sizeof(int32_t))
    {
        assert(false);
        return;
    }
    ack.gapAckBlockCount = ack.gapAckBlockCount.get() + 1;
    block.start = startTsn - ack.cumulativeTsnAck;
    block.end = endTsn - 1 - ack.cumulativeTsnAck;
    ack.header.length = ack.header.length + sizeof(block);
}

// make sure you added all acks before starting to add duplicates
void SackBuilder::addDuplicate(uint32_t transmissionSequenceNumber)
{
    auto& block = ack.data()[ack.gapAckBlockCount + ack.gapDuplicateCount];
    if (&block + 1 > ack.data() + _areaSize / sizeof(int32_t))
    {
        assert(false);
        return;
    }
    block = transmissionSequenceNumber;
    ack.gapDuplicateCount = ack.gapDuplicateCount.get() + 1;
    ack.header.length = ack.header.length + sizeof(block);
}

PaddingParameter::PaddingParameter() : ChunkParameter(ChunkParameterType::Padding) {}

void PaddingParameter::clearPadding(size_t totalPadding)
{
    assert(totalPadding % 4 == 0);
    assert(totalPadding > HEADER_SIZE);
    std::memset(&length + 1, 0, totalPadding - HEADER_SIZE);
    length = totalPadding;
}

#define CODEMSG(x)                                                                                                     \
    case x:                                                                                                            \
        return #x

#define DEFAULTCODEMSG(x)                                                                                              \
    case x:                                                                                                            \
    default:                                                                                                           \
        return #x

const char* toString(ErrorCause code)
{
    switch (code)
    {
        CODEMSG(InvalidStreamIdentifier);
        CODEMSG(MissingMandatoryParameter);
        CODEMSG(StaleCookieError);
        CODEMSG(OutOfResource);
        CODEMSG(UnresolvableAddress);
        CODEMSG(UnrecognizedChunkType);
        CODEMSG(InvalidMandatoryParameter);
        CODEMSG(UnrecognizedParameters);
        CODEMSG(NoUserData);
        CODEMSG(CookieReceivedWhileShuttingDown);
        CODEMSG(RestartOfAnAssociationWithNewAddresses);
        CODEMSG(UserInitiatedAbort);
        DEFAULTCODEMSG(ProtocolError);
    }
}
} // namespace sctp
