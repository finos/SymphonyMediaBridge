#pragma once

#include "utils/ByteOrder.h"
#include "utils/TlvIterator.h"

namespace sctp
{

const size_t SCTP_MTU = 512;

enum ChunkParameterType : uint16_t
{
    HeartbeatInfo = 1,

    V4Address = 5,
    V6Address = 6,
    StateCookie = 7,
    UnrecognizedParameter = 8,
    CookiePreservative = 9,
    HostName = 11,
    SupportedAddressTypes = 12,

    SsnResetOutbound = 13, // https://tools.ietf.org/html/rfc6525
    SsnResetInbound,
    SsnResetAll,
    ReconfigResponse,
    AddOutboundStreams,
    AddInboundStreams,

    Random = 0x8002,
    AuthChunkList, // chunk types that must be signed
    HMACAlgorithm,
    Padding = 0x8005,
    SupportedExtensions = 0x8008, // https://tools.ietf.org/html/rfc5061
    ForwardTsnSupport = 0xC000 // https://tools.ietf.org/html/rfc3758
};

enum ChunkType : uint8_t
{
    DATA = 0,
    INIT,
    INIT_ACK,
    SACK,
    HEARTBEAT,
    HEARTBEAT_ACK,
    ABORT,
    SHUTDOWN,
    SHUTDOWN_ACK,
    ERROR,
    COOKIE_ECHO,
    COOKIE_ACK,
    ECNE,
    SHUTDOWN_COMPLETE = 14,
    AUTH,

    ASCONF_ACK = 128,
    RE_CONFIG = 130, // https://tools.ietf.org/html/rfc6525
    PADDING = 132,
    FORWARDTSN = 192, // TODO must for webRTC https://tools.ietf.org/html/rfc3758
    ASCONF = 193 // address reconfig https://tools.ietf.org/html/rfc5061
};

enum ErrorCause : uint16_t
{
    InvalidStreamIdentifier = 1,
    MissingMandatoryParameter,
    StaleCookieError,
    OutOfResource,
    UnresolvableAddress,
    UnrecognizedChunkType,
    InvalidMandatoryParameter,
    UnrecognizedParameters,
    NoUserData,
    CookieReceivedWhileShuttingDown,
    RestartOfAnAssociationWithNewAddresses,
    UserInitiatedAbort,
    ProtocolError
};

class CommonHeader
{
public:
    CommonHeader() : sourcePort(0), destinationPort(0), verificationTag(0), checksum(0) {}

    nwuint16_t sourcePort;
    nwuint16_t destinationPort;
    nwuint32_t verificationTag;
    uint32_t checksum; // peculiar, but this is how it is read in usctp stack
};

class ChunkParameter
{
public:
    static const size_t HEADER_SIZE = sizeof(uint32_t);
    explicit ChunkParameter(ChunkParameterType paramType) : type(paramType), length(4) {}
    ChunkParameter(const ChunkParameter&) = delete;
    ChunkParameter& operator=(const ChunkParameter&) = delete;

    size_t size() const;
    const uint8_t* data() const;
    uint8_t* data();
    const size_t dataSize() const;
    void zeroPad();

    const nwuint16_t type;
    nwuint16_t length;
};

struct CauseCode : public ChunkParameter
{
    explicit CauseCode(ErrorCause cause) : ChunkParameter(static_cast<ChunkParameterType>(cause)) {}
    CauseCode(ErrorCause cause, const char* reason);
    CauseCode(ErrorCause cause, const uint32_t value);

    std::string getReason() const;
    ErrorCause getCause() const { return static_cast<ErrorCause>(type.get()); }

    void setData(const uint32_t value);

    template <typename T>
    T getData() const
    {
        T value{0};
        if (length < sizeof(T))
        {
            return value;
        }
        std::memcpy(&value, _data, sizeof(T));
        return value;
    }

private:
    char _data[512];
};

class ChunkField
{
protected:
    explicit ChunkField(ChunkType type) { header.type = type; }

public:
    static const size_t BASE_HEADER_SIZE = 2 * sizeof(uint16_t);
    ChunkField(const ChunkField&) = delete;
    ChunkField& operator=(const ChunkField&) = delete;

    utils::TlvCollection<ChunkParameter> params();
    utils::TlvCollectionConst<ChunkParameter> params() const;

    void add(const ChunkParameter& parameter);
    size_t size() const;

    void commitAppendedParameter();
    struct Header
    {
        uint8_t type = 0;
        uint8_t flags = 0;
        nwuint16_t length = nwuint16_t(4);
    };
    Header header;
};

// Since ChunkField cannot have virtual cbegin, we have to put this outside
template <typename T = ChunkParameter, typename CollectionT = utils::TlvCollection<T>>
const T* getParameter(CollectionT& params, ChunkParameterType type)
{
    auto itEnd = params.end();
    for (auto it = params.begin(); it != itEnd; ++it)
    {
        if (it->type == type)
        {
            return reinterpret_cast<const T*>(&*it);
        }
    }
    return nullptr;
}

template <typename T, typename... Args>
T& appendParameter(ChunkField& chunk, Args&&... args)
{
    auto param = new (&*chunk.params().end()) T(std::forward<Args>(args)...);
    return *param;
}

// use this to build new chunks
class GenericChunk : public ChunkField
{
    friend class SctpPacketW;
    explicit GenericChunk(ChunkType type) : ChunkField(type) {}

public:
};

// read only helper for SCTP packet
class SctpPacket
{
public:
    SctpPacket(const void* packet, size_t size);

    utils::TlvCollectionConst<ChunkField> chunks() const;

    const CommonHeader& getHeader() const { return *_commonHeader; };

    template <typename T = ChunkField>
    const T* getChunk(ChunkType type) const
    {
        for (auto& chunk : chunks())
        {
            if (chunk.header.type == type)
            {
                return reinterpret_cast<const T*>(&chunk);
            }
        }
        return nullptr;
    }

    const void* get() const { return _commonHeader; }
    size_t size() const { return _packetSize; }
    bool hasChunk(ChunkType type) const { return getChunk(type) != nullptr; }

    uint32_t calculateCheckSum() const;
    bool isValid() const;

protected:
    size_t _packetSize;
    CommonHeader* _commonHeader;
};

// SCTP packet builder
class SctpPacketW : public SctpPacket
{
public:
    static const size_t HEADER_SIZE = sizeof(CommonHeader);
    SctpPacketW(uint32_t tag, uint16_t srcPort, uint16_t dstPort, uint8_t* dataArea, size_t areaSize);
    SctpPacketW(uint32_t tag, uint16_t srcPort, uint16_t dstPort);

    utils::TlvCollection<ChunkField> chunks();

    void add(const ChunkField& chunk);
    void commitCheckSum() { _commonHeader->checksum = calculateCheckSum(); };

    CommonHeader& getHeader() { return *_commonHeader; }

    size_t capacity() const;

    template <typename T, typename... Args>
    T& appendChunk(Args&&... args)
    {
        auto* chunk = new (&*chunks().end()) T(std::forward<Args>(args)...);
        return *chunk;
    }

    template <typename T, typename... Args>
    void addChunk(Args&&... args)
    {
        new (&*chunks().end()) T(std::forward<Args>(args)...);
        commitAppendedChunk();
    }

    void commitAppendedChunk();
    void clear();

private:
    void checkGuard() const;
    const size_t _areaSize;
    uint8_t _defaultArea[SCTP_MTU + sizeof(uint32_t)]; // for smaller messages
};

// Meta information needed to create an sctp association when this is received in a cookie echo
// make sure your cookie class is properly aligned and packed for int64 / int32

template <typename T>
class CookieParameter : public ChunkParameter
{
public:
    explicit CookieParameter(const T& cookie) : ChunkParameter(ChunkParameterType::StateCookie)
    {
        length = ChunkParameter::HEADER_SIZE + sizeof(T);
        std::memcpy(_cookie, &cookie, sizeof(T)); // otherwise it may align to 64 bit
    }

    T getCookie() const
    {
        T value;
        std::memcpy(value, _cookie, sizeof(T));
        return value;
    }

private:
    uint8_t _cookie[sizeof(T)];
};

class SupportedExtensionsParameter : public ChunkParameter
{
    template <typename T, typename... Args>
    friend T& appendParameter(ChunkField& chunk, Args&&... args);
    SupportedExtensionsParameter() : ChunkParameter(ChunkParameterType::SupportedExtensions){};

public:
    SupportedExtensionsParameter(const SupportedExtensionsParameter&) = delete;
    SupportedExtensionsParameter& operator=(const SupportedExtensionsParameter&) = delete;

    size_t getCount() const { return length - HEADER_SIZE; }
    void add(ChunkType chunkType);
};

class InitChunk : public ChunkField
{
protected:
    friend class SctpPacketW;
    InitChunk() : ChunkField(ChunkType::INIT) { header.length = sizeof(header) + 4 * sizeof(uint32_t); }

public:
    utils::TlvCollection<ChunkParameter> params()
    {
        return utils::TlvCollection<ChunkParameter>(&initTSN + 1, reinterpret_cast<uint8_t*>(&header) + size());
    }
    utils::TlvCollectionConst<ChunkParameter> params() const
    {
        return utils::TlvCollectionConst<ChunkParameter>(&initTSN + 1,
            reinterpret_cast<const uint8_t*>(&header) + size());
    }

    nwuint32_t initTag;
    nwuint32_t advertisedReceiverWindow;
    nwuint16_t outboundStreams;
    nwuint16_t inboundStreams;
    nwuint32_t initTSN;
};

// This will contain a Cookie Parameter
class InitAckChunk : public InitChunk
{
    friend class SctpPacketW;
    InitAckChunk() { header.type = ChunkType::INIT_ACK; }

public:
};

class CookieEchoChunk : public ChunkField
{
    friend class SctpPacketW;
    CookieEchoChunk();

public:
    void setCookie(const ChunkParameter& param);
    void setCookie(const void* cookie, size_t length);

    template <typename T>
    const T getCookie() const
    {
        T result;
        std::memcpy(&result, &header + 1, sizeof(T));
        return result;
    }

private:
    // params not supported in EchoChunk
};

struct AckRange
{
    uint32_t start;
    uint32_t end;
};

// When building sack you have to
// set cumulativeAck
// add acks
// add duplicates
class SelectiveAckChunk : public ChunkField
{
    friend class SctpPacketW;
    SelectiveAckChunk()
        : ChunkField(ChunkType::SACK),
          cumulativeTsnAck(0),
          advertisedReceiverWindow(0),
          gapAckBlockCount(0),
          gapDuplicateCount(0)
    {
        header.length = HEADER_SIZE;
    }

public:
    static const size_t HEADER_SIZE = ChunkField::BASE_HEADER_SIZE + 3 * sizeof(uint32_t);
    struct RawGapAckBlock
    {
        nwuint16_t start;
        nwuint16_t end;
    };

    AckRange getAck(int index) const;
    uint32_t getDuplicate(int index) const;

    nwuint32_t cumulativeTsnAck; // this chunk tsn has been received and all bytes in that chunk
    nwuint32_t advertisedReceiverWindow;
    nwuint16_t gapAckBlockCount;
    nwuint16_t gapDuplicateCount;

private:
    friend class SackBuilder;
    uint32_t* data() { return reinterpret_cast<uint32_t*>(&gapDuplicateCount + 1); }
    const uint32_t* data() const { return reinterpret_cast<const uint32_t*>(&gapDuplicateCount + 1); }
};

class SackBuilder
{
public:
    SackBuilder(uint8_t* data, size_t areaSize);
    SackBuilder(SelectiveAckChunk* ack, size_t areaSize);

    void addAck(uint32_t startTsn, uint32_t endTsn);
    void addDuplicate(uint32_t transmissionSequenceNumber);
    void clear() { ack.header.length = SelectiveAckChunk::HEADER_SIZE; }

    size_t size() const { return ack.size(); }

    SelectiveAckChunk& ack;

private:
    const size_t _areaSize;
};

class AbortChunk : public ChunkField
{
    friend class SctpPacketW;
    explicit AbortChunk(bool tagIsReflective) : ChunkField(ChunkType::ABORT)
    {
        if (tagIsReflective)
        {
            header.flags = 1;
        }
        else
        {
            header.flags = 0;
        }
    }

public:
    utils::TlvCollection<CauseCode> causes()
    {
        return utils::TlvCollection<CauseCode>(&header + 1, reinterpret_cast<uint8_t*>(&header) + size());
    }
    utils::TlvCollectionConst<CauseCode> causes() const
    {
        return utils::TlvCollectionConst<CauseCode>(&header + 1, reinterpret_cast<const uint8_t*>(&header) + size());
    }
};

class PayloadDataChunk : public ChunkField
{
    friend class SctpPacketW;
    PayloadDataChunk();
    PayloadDataChunk(uint16_t streamId, uint16_t sequenceNumber, uint32_t payloadProtocol, uint32_t tsn);

public:
    static const size_t HEADER_SIZE = ChunkField::BASE_HEADER_SIZE + 3 * sizeof(uint32_t);

    void setUnordered() { header.flags |= 0x04; }
    void setFragmentBegin() { header.flags |= 0x02; }
    void setFragmentEnd() { header.flags |= 0x01; }

    bool isBegin() const { return (header.flags & 0x02) == 0x02; }
    bool isEnd() const { return (header.flags & 0x01) == 0x01; }

    void clear();
    void writeData(const void* data, size_t length, bool fragmentBegin, bool fragmentEnd);
    const uint8_t* data() const { return _data; }
    size_t payloadSize() const { return header.length - HEADER_SIZE; }

    nwuint32_t transmissionSequenceNumber;
    nwuint16_t streamId;
    nwuint16_t streamSequenceNumber;
    nwuint32_t payloadProtocol;

private:
    uint8_t _data[4];
};

const size_t PAYLOAD_DATA_OVERHEAD = SctpPacketW::HEADER_SIZE + PayloadDataChunk::HEADER_SIZE;

class HeartbeatInfoParameter : public ChunkParameter
{
public:
    HeartbeatInfoParameter()
        : ChunkParameter(ChunkParameterType::HeartbeatInfo),
          sequenceNumber(0),
          mtu(0),
          timestamp(0),
          nonce(0)
    {
        length = HEADER_SIZE;
    }

    static const size_t HEADER_SIZE = 5 * sizeof(uint32_t) + sizeof(ChunkParameter);

    nwuint16_t sequenceNumber;
    nwuint16_t mtu;
    nwuint64_t timestamp;
    nwuint64_t nonce;
};

class PaddingParameter : public ChunkParameter
{
public:
    PaddingParameter();
    void clearPadding(size_t padding);
};

bool isCookieEcho(const void* data, size_t length);
bool isSctpInit(const void* data, size_t length);
uint32_t getAssociationTag(const void* data, size_t length);
uint16_t getDestinationPort(const void* data, size_t length);
inline int32_t diff(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(b - a);
}

const char* toString(ErrorCause cause);
} // namespace sctp
