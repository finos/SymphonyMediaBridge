#pragma once

#include "bwe/RateController.h"
#include "concurrency/MpmcHashmap.h"
#include "dtls/SrtpClient.h"
#include "dtls/SslWriteBioListener.h"
#include "ice/IceSession.h"
#include "logger/Logger.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "rtp/SendTimeDial.h"
#include "sctp/SctpAssociation.h"
#include "sctp/SctpServerPort.h"
#include "transport/Endpoint.h"
#include "transport/RtcTransport.h"
#include "transport/RtcpReportProducer.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "utils/Optional.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace logger
{
class PacketLoggerThread;
}

namespace bwe
{
class BandwidthEstimator;
struct Config;
} // namespace bwe

namespace sctp
{
struct SctpConfig;
} // namespace sctp

namespace transport
{

class TransportImpl : public RtcTransport,
                      private SslWriteBioListener,
                      public Endpoint::IEvents,
                      public ice::IceSession::IEvents,
                      public SrtpClient::IEvents,
                      public sctp::DatagramTransport,
                      public sctp::SctpServerPort::IEvents,
                      public sctp::SctpAssociation::IEvents,
                      public ServerEndpoint::IEvents,
                      private RtcpReportProducer::RtcpSender
{
public:
    TransportImpl(jobmanager::JobManager& jobmanager,
        SrtpClientFactory& srtpClientFactory,
        const size_t endpointIdHash,
        const config::Config& config,
        const sctp::SctpConfig& sctpConfig,
        const ice::IceConfig& iceConfig,
        const ice::IceRole iceRole,
        const bwe::Config& bweConfig,
        const bwe::RateControllerConfig& rateControllerConfig,
        const Endpoints& rtpEndPoints,
        const ServerEndpoints& tcpEndPoints,
        TcpEndpointFactory* tcpEndpointFactory,
        memory::PacketPoolAllocator& allocator,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation);

    TransportImpl(jobmanager::JobManager& jobmanager,
        SrtpClientFactory& srtpClientFactory,
        const size_t endpointIdHash,
        const config::Config& config,
        const sctp::SctpConfig& sctpConfig,
        const bwe::Config& bweConfig,
        const bwe::RateControllerConfig& rateControllerConfig,
        const Endpoints& rtpEndPoints,
        const Endpoints& rtcpEndPoints,
        memory::PacketPoolAllocator& allocator);

    ~TransportImpl() override;

public: // Transport
    bool isInitialized() const override { return _isInitialized; }
    const logger::LoggableId& getLoggableId() const override { return _loggableId; }
    size_t getId() const override { return _loggableId.getInstanceId(); }
    size_t getEndpointIdHash() const override { return _endpointIdHash; };

    /** Called from httpd threads */
    void stop() override;
    bool isRunning() const override { return _isRunning && _isInitialized; }
    bool hasPendingJobs() const override { return _jobCounter.load() > 0; }
    std::atomic_uint32_t& getJobCounter() override { return _jobCounter; }

    /** Called from Transport thread threads*/
    void protectAndSend(memory::UniquePacket packet) override;
    bool unprotect(memory::Packet& packet) override;
    void removeSrtpLocalSsrc(const uint32_t ssrc) override;
    bool setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter) override;
    void setRtxProbeSource(const uint32_t ssrc, uint32_t* sequenceCounter, const uint16_t payloadType) override;

    /** Called from httpd threads */
    bool isGatheringComplete() const override;
    /** Called from httpd threads */
    ice::IceCandidates getLocalCandidates() override;
    /** Called from httpd threads */
    std::pair<std::string, std::string> getLocalIceCredentials() override;

    /** Called from httpd threads. Must be set before DtlsFingerprint otherwise DTLS handshake will fail */
    bool setRemotePeer(const SocketAddress& target) override;
    const SocketAddress& getRemotePeer() const override { return _peerRtpPort; }
    void setRemoteIce(const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::AudioPacketPoolAllocator& allocator) override;
    void addRemoteIceCandidate(const ice::IceCandidate& candidate) override;
    void asyncSetRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide) override;
    void asyncDisableSrtp() override;
    SocketAddress getLocalRtpPort() const override;
    /**
     * Called from engine thread.
     * Data receiver must live as long as Transport's owner. See start().
     */
    void setDataReceiver(DataReceiver* dataReceiver) override;

    /** Called from worker threads*/
    bool isConnected() override;

    bool isDtlsClient() override
    {
        assert(_srtpClient);
        return _srtpClient->isDtlsClient();
    }

    /**
     * Called right after ctr from the same thread.
     * Set the reference to owner's reference counter.
     * Transport object have an owner - object who creates and eventually delete Transport.
     * Owner can be prevented from deletion by incrementing its reference counter.
     */
    bool start() override;

    bool isIceEnabled() const override { return !!_rtpIceSession; }

    void connect() override;

    jobmanager::JobQueue& getJobQueue() override { return _jobQueue; }

    uint32_t getSenderLossCount() const override;
    uint32_t getUplinkEstimateKbps() const override;
    uint32_t getDownlinkEstimateKbps() const override;
    uint32_t getPacingQueueCount() const override;
    uint32_t getRtxPacingQueueCount() const override;
    uint64_t getRtt() const override;
    PacketCounters getCumulativeReceiveCounters(uint32_t ssrc) const override;
    PacketCounters getCumulativeAudioReceiveCounters() const override;
    PacketCounters getCumulativeVideoReceiveCounters() const override;
    PacketCounters getAudioReceiveCounters(uint64_t idleTimestamp) const override;
    PacketCounters getVideoReceiveCounters(uint64_t idleTimestamp) const override;
    PacketCounters getAudioSendCounters(uint64_t idleTimestamp) const override;
    PacketCounters getVideoSendCounters(uint64_t idleTimestamp) const override;
    void getReportSummary(std::unordered_map<uint32_t, ReportSummary>& outReportSummary) const override;
    uint64_t getInboundPacketCount() const override;

    void setAudioPayloads(uint8_t payloadType,
        utils::Optional<uint8_t> telephoneEventPayloadType,
        uint32_t rtpFrequency) override;
    void setAbsSendTimeExtensionId(uint8_t extensionId) override;

    bool sendSctp(uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length) override;
    uint16_t allocateOutboundSctpStream() override;
    void setSctp(uint16_t localPort, uint16_t remotePort) override;
    void connectSctp() override;
    void runTick(uint64_t timestamp) override;
    ice::IceSession::State getIceState() const override { return _iceState; };
    SrtpClient::State getDtlsState() const override { return _dtlsState; };
    utils::Optional<ice::TransportType> getSelectedTransportType() const override { return _transportType.load(); }

    void setTag(const char* tag) override;
    const char* getTag() const override { return _tag; };

    uint64_t getLastReceivedPacketTimestamp() const override { return _lastReceivedPacketTimestamp; }

    void getSdesKeys(std::vector<srtp::AesKey>& sdesKeys) const override;
    void asyncSetRemoteSdesKey(const srtp::AesKey& key) override;

private: // SslWriteBioListener
    // Called from Transport serial thread
    int32_t sendDtls(const char* buffer, uint32_t length) override;
    void onSrtpStateChange(SrtpClient* srtpClient, SrtpClient::State state) override;

    /** Called from transport's serial jobmanager */
    void onIceStateChanged(ice::IceSession* session, ice::IceSession::State state) override;
    void onIceCompleted(ice::IceSession* session) override;
    void onIceCandidateChanged(ice::IceSession* session,
        ice::IceEndpoint* endpoint,
        const SocketAddress& sourcePort) override;
    void onIceDiscardCandidate(ice::IceSession* session,
        ice::IceEndpoint* endpoint,
        const transport::SocketAddress& sourcePort) override;

    // end point callbacks
    void onRegistered(Endpoint& endpoint) override;
    void onServerPortRegistered(ServerEndpoint& endpoint) override;
    void onUnregistered(Endpoint& endpoint) override;
    void onRtpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void onDtlsReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void onRtcpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void onIceReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void onTcpDisconnect(Endpoint& endpoint) override;

    void onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    // DataGramTransport for sctp
    virtual bool sendSctpPacket(const void* data, size_t length) override;
    virtual bool onSctpInitReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& sctpPacket,
        uint64_t timestamp,
        uint16_t& inboundStreams,
        uint16_t& outboundStreams) override;
    virtual void onSctpCookieEchoReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& packet,
        uint64_t timestamp) override;
    virtual void onSctpReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& sctpPacket,
        uint64_t timestamp) override;
    virtual void onSctpStateChanged(sctp::SctpAssociation* session, sctp::SctpAssociation::State state) override;
    virtual void onSctpFragmentReceived(sctp::SctpAssociation* session,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* buffer,
        size_t length,
        uint64_t timestamp) override;
    virtual void onSctpEstablished(sctp::SctpAssociation* session) override;
    virtual void onSctpClosed(sctp::SctpAssociation* session) override;
    virtual void onSctpChunkDropped(sctp::SctpAssociation* session, size_t size) override;

    void internalDtlsReceived(Endpoint& endpoint,
        SocketAddress source,
        memory::UniquePacket packet,
        uint64_t timestamp);
    void internalIceReceived(Endpoint& endpoint, SocketAddress source, memory::UniquePacket packet, uint64_t timestamp);
    void internalRtpReceived(Endpoint& endpoint, SocketAddress source, memory::UniquePacket packet, uint64_t timestamp);
    void internalRtcpReceived(Endpoint& endpoint,
        SocketAddress source,
        memory::UniquePacket packet,
        uint64_t timestamp);
    void internalIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
        SocketAddress source,
        memory::UniquePacket packet,
        uint64_t timestamp);
    void internalUnregisterEndpoints();
    void internalShutdown();

    void onServerPortUnregistered(ServerEndpoint& endpoint) override;

private:
    friend class IceSetRemoteJob;
    friend class ConnectJob;
    friend class ConnectSctpJob;
    friend class RunTickJob;
    friend class PacketReceiveJob;

    enum class DrainPacingBufferMode
    {
        DrainAll,
        UseBudget,
    };

    void protectAndSendRtp(uint64_t timestamp, memory::UniquePacket packet);
    void doProtectAndSend(uint64_t timestamp,
        memory::UniquePacket packet,
        const SocketAddress& target,
        Endpoint* endpoint);
    void sendPadding(uint64_t timestamp);

    void processRtcpReport(const rtp::RtcpHeader& packet,
        uint64_t timestamp,
        std::__1::chrono::system_clock::time_point wallClock);

    void doSetRemoteIce(const memory::AudioPacket& credentials, const memory::AudioPacket& candidates);

    void doSetRemoteDtls(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide);

    bool doBandwidthEstimation(uint64_t receiveTime, const utils::Optional<uint32_t>& absSendTime, uint32_t packetSize);
    void doConnect();
    void doConnectSctp();
    void doRunTick(uint64_t timestamp);

    void appendRemb(memory::Packet& rtcpPacket,
        const uint64_t timestamp,
        uint32_t senderSsrc,
        const uint32_t* activeInbound,
        int activeInboundCount);

    void sendReports(uint64_t timestamp, bool rembReady = false);
    void sendRtcp(memory::UniquePacket rtcpPacket, const uint64_t timestamp) override;

    void onSendingRtcp(const memory::Packet& rtcpPacket, uint64_t timestamp);

    RtpReceiveState& getInboundSsrc(uint32_t ssrc, uint32_t rtpFrequency);
    RtpSenderState& getOutboundSsrc(uint32_t ssrc, uint32_t rtpFrequency);

    void onTransportConnected();
    void drainPacingBuffer(uint64_t timestamp, DrainPacingBufferMode);
    memory::UniquePacket tryFetchPriorityPacket(size_t budget);

    std::atomic_bool _isInitialized;
    logger::LoggableId _loggableId;
    size_t _endpointIdHash;
    const config::Config& _config;

    std::unique_ptr<SrtpClient> _srtpClient;
    std::unique_ptr<ice::IceSession> _rtpIceSession;

    Endpoints _rtpEndpoints;
    Endpoints _rtcpEndpoints;
    ServerEndpoints _tcpServerEndpoints;
    TcpEndpointFactory* _tcpEndpointFactory;

    std::atomic_uint32_t _jobCounter;
    utils::SsrcGenerator _randomGenerator;

    transport::SocketAddress _peerRtpPort;
    transport::SocketAddress _peerRtcpPort;
    Endpoint* _selectedRtp;
    Endpoint* _selectedRtcp;

    std::atomic<DataReceiver*> _dataReceiver;

    memory::PacketPoolAllocator& _mainAllocator; // for DTLS and RTCP

    jobmanager::JobQueue _jobQueue;

    struct ChannelMetrics
    {
        ChannelMetrics(uint32_t initialEstimateKbps)
            : packetCount(0),
              bytesCount(0),
              estimatedKbps(initialEstimateKbps),
              estimatedKbpsMin(initialEstimateKbps),
              estimatedKbpsMax(initialEstimateKbps)
        {
        }

        std::atomic_uint32_t packetCount;
        std::atomic_uint32_t bytesCount;
        std::atomic_uint32_t estimatedKbps;
        std::atomic_uint32_t estimatedKbpsMin;
        std::atomic_uint32_t estimatedKbpsMax;
    };
    ChannelMetrics _inboundMetrics;
    ChannelMetrics _outboundMetrics;

    struct PacingQueueStats
    {
        PacingQueueStats() : pacingQueueSize(0), rtxPacingQueueSize(0) {}

        std::atomic_uint32_t pacingQueueSize;
        std::atomic_uint32_t rtxPacingQueueSize;
    } _pacingQueueStats;

    uint32_t _outboundRembEstimateKbps;
    utils::RateTracker<10> _sendRateTracker; // B/ns
    uint64_t _lastLogTimestamp;
    std::atomic_uint32_t _rttNtp;

    concurrency::MpmcHashmap32<uint32_t, RtpSenderState> _outboundSsrcCounters;
    concurrency::MpmcHashmap32<uint32_t, RtpReceiveState> _inboundSsrcCounters;

    std::atomic_bool _isRunning;

    struct
    {
        bool containsPayload(uint8_t pt) const
        {
            return pt == payloadType || (telephoneEventPayloadType.isSet() && telephoneEventPayloadType.get() == pt);
        }

        uint8_t payloadType;
        utils::Optional<uint8_t> telephoneEventPayloadType;
        uint32_t rtpFrequency;
    } _audio;
    uint8_t _absSendTimeExtensionId;
    uint16_t _videoRtxPayloadType;

    const sctp::SctpConfig& _sctpConfig;
    utils::Optional<uint16_t> _remoteSctpPort;
    std::unique_ptr<sctp::SctpServerPort> _sctpServerPort;
    std::unique_ptr<sctp::SctpAssociation> _sctpAssociation;

    rtp::SendTimeDial _sendTimeTracker;
    std::unique_ptr<bwe::BandwidthEstimator> _bwe;

    struct RtcpMaintenance
    {
        RtcpMaintenance() : lastSendTime(0), lastReportedEstimateKbps(0) {}

        uint64_t lastSendTime;
        uint32_t lastReportedEstimateKbps;
    } _rtcp;

    bwe::RateController _rateController;
    uint32_t _rtxProbeSsrc;
    uint32_t* _rtxProbeSequenceCounter;

    using PacingQueue = memory::RandomAccessBacklog<memory::UniquePacket, 512>;
    PacingQueue _pacingQueue;
    PacingQueue _rtxPacingQueue;
    std::atomic_bool _pacingInUse;

    std::unique_ptr<logger::PacketLoggerThread> _packetLogger;
    std::atomic<ice::IceSession::State> _iceState;
    std::atomic<SrtpClient::State> _dtlsState;
    std::atomic<bool> _isConnected;
    std::atomic<utils::Optional<ice::TransportType>> _transportType;

    RtcpReportProducer _rtcpProducer;
#ifdef DEBUG
public:
    concurrency::MutexGuard _singleThreadMutex;
#endif

    char _tag[16];

    bool _uplinkEstimationEnabled;
    bool _downlinkEstimationEnabled;
    std::atomic_uint64_t _lastReceivedPacketTimestamp;
    uint64_t _lastTickJobStartTimestamp;
};

} // namespace transport
