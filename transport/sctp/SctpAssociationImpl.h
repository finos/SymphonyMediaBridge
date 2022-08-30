#include "SctpAssociation.h"
#include "SctpTimer.h"
#include "Sctprotocol.h"
#include "logger/Logger.h"
#include "memory/RingAllocator.h"
#include <list>
#include <map>
#include <unordered_map>
namespace sctp
{
class SctpServerPort;

// Session state for a connection between client and peer
//
// Unsupported features:
//  - graceful disconnect
//  - multi homed hosts
//  - unordered flag delivery
//  - streams
class SctpAssociationImpl : public SctpAssociation
{
    struct SentDataChunk
    {
        SentDataChunk()
            : transmitTime(0),
              size(0),
              transmissionSequenceNumber(0),
              streamId(0),
              streamSequenceNumber(0),
              payloadProtocol(0),
              transmitCount(0),
              nackCount(0),
              fragmentBegin(false),
              fragmentEnd(false),
              reserved0(0),
              reserved1(0)
        {
        }

        SentDataChunk(uint16_t streamId,
            uint32_t payloadProtocol,
            uint16_t streamSequenceNumber,
            bool fragmentBegin,
            bool fragmentEnd,
            uint32_t transmissionSequenceNumber,
            const void* payload,
            size_t size);

        uint64_t transmitTime;
        const uint32_t size;
        const uint32_t transmissionSequenceNumber;
        const uint16_t streamId;
        const uint16_t streamSequenceNumber;
        const uint32_t payloadProtocol;
        uint16_t transmitCount;
        uint16_t nackCount;
        const bool fragmentBegin;
        const bool fragmentEnd;
        uint16_t reserved0;
        const uint32_t reserved1;

        uint32_t fullSize() const { return size + PayloadDataChunk::HEADER_SIZE; }
        uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
        const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this + 1); }
    };
    struct ReceivedDataChunk
    {
        ReceivedDataChunk()
            : receiveTime(0),
              size(0),
              transmissionSequenceNumber(0),
              streamId(0),
              streamSequenceNumber(0),
              payloadProtocol(0),
              receiveCount(0),
              fragmentBegin(true),
              fragmentEnd(true)
        {
        }
        ReceivedDataChunk(const PayloadDataChunk& chunk, uint64_t timestamp);

        const uint64_t receiveTime;
        const size_t size;
        const uint32_t transmissionSequenceNumber;
        const uint16_t streamId;
        const uint16_t streamSequenceNumber;
        const uint32_t payloadProtocol;
        uint16_t receiveCount;
        const bool fragmentBegin;
        const bool fragmentEnd;

        uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
    };

    class LessReceivedDataChunk
    {
    public:
        bool operator()(uint32_t tsnA, uint32_t tsnB) const { return diff(tsnA, tsnB) > 0; }
    };
    typedef std::map<uint32_t, ReceivedDataChunk*, LessReceivedDataChunk> InboundChunkList;

public:
    SctpAssociationImpl(size_t logId,
        SctpServerPort& transport,
        uint16_t remotePort,
        IEvents* listener,
        const SctpConfig& config);

    SctpAssociationImpl(size_t logId,
        SctpServerPort& transport,
        const SctpPacket& cookieEcho,
        IEvents* listener,
        const SctpConfig& config);

    void onCookieEcho(const SctpPacket& sctpPacket, const uint64_t timestamp) override;

    void connect(uint16_t inboundStreamCount, uint16_t outboundStreamCount, uint64_t timestamp) override;
    uint16_t allocateStream() override;
    bool sendMessage(uint16_t streamId,
        uint32_t payloadProtocol,
        const void* payloadData,
        size_t length,
        uint64_t timestamp) override;
    size_t outboundPendingSize() const override;
    int64_t nextTimeout(uint64_t timestamp) override;
    int64_t processTimeout(uint64_t timestamp) override;
    State getState() const override { return _state.load(); };

    int64_t onPacketReceived(const SctpPacket&, uint64_t timestamp) override;

    void sendErrorResponse(const SctpPacket& request, const CauseCode& errorCause) override;

    std::tuple<uint16_t, uint16_t> getPortPair() const override;
    std::tuple<uint32_t, uint32_t> getTags() const override;

    void startMtuProbing(uint64_t timestamp) override;
    uint32_t getScptMTU() const override { return _mtu.current; }

    void setAdvertisedReceiveWindow(uint32_t size) override;

    void close() override;

    size_t getStreamCount() const override { return _streams.size(); }

private:
    void setState(State newState);
    void sendInit();
    void retransmitCookie();
    void sendMtuProbe(uint64_t timestamp);
    void processOutboundChunks(uint64_t timestamp);
    void onSackReceived(const SctpPacket& sctpPacket, const SelectiveAckChunk& chunk, uint64_t timestamp);
    void handleFastRetransmits(uint64_t timestamp);
    void prepareSack(uint64_t timestamp);
    bool gatherReceivedFragments(uint64_t timestamp);
    void reportFragment(InboundChunkList::iterator chunkHeadIt, size_t fragmentSize, uint64_t timestamp);
    uint32_t getFlightSize(uint64_t timestamp) const;
    void updateRetransmitTimer(uint64_t timestamp, bool retransmitsPerformed);

    void onInitAck(const SctpPacket& packet, const InitAckChunk& initAckChunk, uint64_t timestamp);
    void onCookieAckReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onAbortReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onErrorReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onShutDownReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onShutDownAckReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onShutDownCompleteReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onDataReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onUnexpectedCookieEcho(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onUnexpectedInitReceived(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onHeartbeatRequest(const SctpPacket& sctpPacket, uint64_t timestamp);
    void onHeartbeatResponse(const SctpPacket& sctpPacket, uint64_t timestamp);

    logger::LoggableId _loggableId;
    const SctpConfig& _config;
    SctpServerPort& _transport;
    IEvents* _listener;

    std::atomic<State> _state;
    // Transmission Control Block
    struct TransmissionControlBlock
    {
        TransmissionControlBlock(uint16_t port_,
            uint32_t tag_,
            uint32_t advertisedReceiveWindow,
            uint16_t inboundStreams_,
            uint16_t outboundStreams_);

        uint16_t port;
        uint32_t tag;
        uint32_t tieTag;
        uint32_t tsn;
        uint32_t advertisedReceiveWindow;
        uint16_t inboundStreamCount;
        uint16_t outboundStreamCount;
        uint32_t cumulativeAck; // highest sent to our knowledge
    };
    TransmissionControlBlock _local;
    TransmissionControlBlock _peer;

    struct GenericCookie
    {
        size_t maxSize() const;
        void set(const ChunkParameter& param);

        uint8_t cookie[512] = {0};
        size_t length = 0;
    };

    struct ConnectionEstablishment
    {
        explicit ConnectionEstablishment(const SctpConfig& config);

        int retransmitCount;
        Timer initTimer;
        Timer cookieTimer;
        GenericCookie echoedCookie;
        double timerBackOff;
    } _connect;

    struct RTT // in ns
    {
    public:
        RTT(const SctpConfig& config);
        void update(uint64_t rttns);
        double getPeak() const;

    private:
        const SctpConfig& _config;
        double _peak;
        double _smoothed;
        double _meanDeviation;
    } _rtt;

    struct MTU
    {
        explicit MTU(uint32_t assumedMTU, uint32_t maxMTU_)
            : probing(false),
              current(assumedMTU),
              probeTimer(1000, 3000),
              selectedProbe(MTU_SIZES),
              transmitCount(0),
              maxMTU(maxMTU_)
        {
        }

        bool probing;
        uint32_t current;
        Timer probeTimer;
        const size_t* selectedProbe;
        int transmitCount;
        static const size_t MTU_SIZES[];
        uint32_t maxMTU;
        void pickInitialProbe();
    } _mtu; // for SCTP layer

    std::list<SentDataChunk*> _outboundDataChunks;
    memory::RingAllocator _outboundBuffer;

    InboundChunkList _inboundDataChunks;
    memory::RingAllocator _inboundBuffer;

    struct Sacks
    {
        Sacks() : prepared(false) { pendingAck[0] = 0; }

        uint8_t pendingAck[2048];
        bool prepared;
    };
    Sacks _ack;

    class CongestionControl
    {
    public:
        CongestionControl(const SctpConfig& config, const logger::LoggableId& logId);
        void reset(uint32_t mtu, size_t slowStartThreshold_);

        void onSackReceived(uint64_t timestamp,
            uint32_t mtu,
            uint32_t flightSizeBeforeAck,
            uint32_t bytesAcked,
            bool allPendingDataAcked);

        void onIdle(uint64_t timestamp, uint32_t mtu);
        void onPacketLoss(uint64_t timestamp, uint32_t mtu);
        void onTransmitTimeout(uint64_t timestamp, uint32_t mtu);

        uint64_t getRetransmitTimeout() const;
        void doubleRetransmitTimeout();
        void resetRetransmitTimeout();

        uint32_t congestionWindow;
        size_t slowStartThreshold;
        Timer idleTimer;
        Timer retransmitTimer;
        // TODO Timer for delivering next burst in case we have not received sack yet
        bool inFastRecovery;
        uint32_t fastRecoveryExitPoint;
        uint64_t retransmitTimeout0; // base retransmit timeout value

    private:
        double _retransmitBackOffFactor;

        size_t _partialBytesAcked;
        const logger::LoggableId& _loggableId;
        const SctpConfig& _config;
    } _flow;

    struct Stream
    {
        Stream(uint16_t streamId_) : streamId(streamId_), sequenceCounter(0) {}

        uint16_t streamId;
        uint16_t sequenceCounter;
    };
    uint16_t _streamIdCounter;

    std::unordered_map<uint16_t, Stream> _streams; // ids chosen odd/even
};

} // namespace sctp
