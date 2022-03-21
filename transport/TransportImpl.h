#pragma once

#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "dtls/SrtpClient.h"
#include "dtls/SrtpClientFactory.h"
#include "dtls/SslDtls.h"
#include "dtls/SslWriteBioListener.h"
#include "ice/IceSession.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"
#include "rtp/SendTimeDial.h"
#include "sctp/SctpAssociation.h"
#include "sctp/SctpConfig.h"
#include "sctp/SctpServerPort.h"
#include "transport/RtcSocket.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "transport/UdpEndpoint.h"
#include "utils/ScopedIncrement.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
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
class SctpServerPort;
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
                      public ServerEndpoint::IEvents
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
        const std::vector<Endpoint*>& rtpEndPoints,
        const std::vector<ServerEndpoint*>& tcpEndPoints,
        TcpEndpointFactory* tcpEndpointFactory,
        memory::PacketPoolAllocator& allocator);

    TransportImpl(jobmanager::JobManager& jobmanager,
        SrtpClientFactory& srtpClientFactory,
        const size_t endpointIdHash,
        const config::Config& config,
        const sctp::SctpConfig& sctpConfig,
        const bwe::Config& bweConfig,
        const bwe::RateControllerConfig& rateControllerConfig,
        const std::vector<Endpoint*>& rtpEndPoints,
        const std::vector<Endpoint*>& rtcpEndPoints,
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
    void protectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator) override;
    bool unprotect(memory::Packet* packet) override;
    void removeSrtpLocalSsrc(const uint32_t ssrc) override;
    bool setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter) override;
    void setRtxProbeSource(uint32_t ssrc, uint32_t* sequenceCounter) override;

    /** Called from httpd threads */
    bool isGatheringComplete() const override;
    /** Called from httpd threads */
    ice::IceCandidates getLocalCandidates() override;
    /** Called from httpd threads */
    std::pair<std::string, std::string> getLocalCredentials() override;

    /** Called from httpd threads. Must be set before DtlsFingerprint otherwise DTLS handshake will fail */
    bool setRemotePeer(const SocketAddress& target) override;
    const SocketAddress& getRemotePeer() const override { return _peerRtpPort; }
    void setRemoteIce(const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::PacketPoolAllocator& allocator) override;
    void setRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide) override;
    void disableDtls() override;
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
    bool isDtlsEnabled() const override { return _dtlsEnabled; }

    void connect() override;

    jobmanager::JobQueue& getJobQueue() override { return _jobQueue; }

    uint32_t getSenderLossCount() const override;
    uint32_t getUplinkEstimateKbps() const override;
    uint32_t getDownlinkEstimateKbps() const override;
    uint32_t getPacingQueueCount() const override;
    uint32_t getRtxPacingQueueCount() const override;
    uint64_t getRtt() const override;
    PacketCounters getCumulativeReceiveCounters(uint32_t ssrc) const override;
    PacketCounters getAudioReceiveCounters(uint64_t idleTimestamp) const override;
    PacketCounters getVideoReceiveCounters(uint64_t idleTimestamp) const override;
    PacketCounters getAudioSendCounters(uint64_t idleTimestamp) const override;
    PacketCounters getVideoSendCounters(uint64_t idleTimestamp) const override;
    void getReportSummary(std::unordered_map<uint32_t, ReportSummary>& outReportSummary) const override;

    void setAudioPayloadType(uint8_t payloadType, uint32_t rtpFrequency) override;
    void setAbsSendTimeExtensionId(uint8_t extensionId) override;
    bool sendSctp(uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length) override;
    uint16_t allocateOutboundSctpStream() override;
    void setSctp(uint16_t localPort, uint16_t remotePort) override;
    void connectSctp() override;
    void runTick(uint64_t timestamp) override;

    bool isUsingRtpTcpCandidate() override { return _selectedRtp && _selectedRtp->getTransportType() != ice::TransportType::UDP; }
    bool isUsingRtcpTcpCandidate() override { return _selectedRtcp && _selectedRtcp->getTransportType() != ice::TransportType::UDP; }

public: // SslWriteBioListener
    // Called from Transport serial thread
    int32_t sendDtls(const char* buffer, uint32_t length) override;
    void onDtlsStateChange(SrtpClient* srtpClient, SrtpClient::State state) override;

public:
    /** Called from transport's serial jobmanager */
    void onIceStateChanged(ice::IceSession* session, ice::IceSession::State state) override;
    void onIceCompleted(ice::IceSession* session) override;
    void onIcePreliminary(ice::IceSession* session,
        ice::IceEndpoint* endpoint,
        const SocketAddress& sourcePort) override;

public: // end point callbacks
    void onUnregistered(Endpoint& endpoint) override;
    void onRtpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) override;

    void onDtlsReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) override;

    void onRtcpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) override;

    void onIceReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) override;

    void onPortClosed(Endpoint& endpoint) override;

    void onIceDisconnect(Endpoint& endpoint);

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
        uint64_t timetamp) override;
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
        const SocketAddress& source,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        uint64_t timestamp);
    void internalIceReceived(Endpoint& endpoint,
        const SocketAddress& source,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        uint64_t timestamp);
    void internalRtpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        uint64_t timestamp);
    void internalRtcpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        uint64_t timestamp);

    void onServerPortClosed(ServerEndpoint& endpoint) override {}
    void onServerPortUnregistered(ServerEndpoint& endpoint) override;

private:
    friend class IceSetRemoteJob;
    friend class ConnectJob;
    friend class ConnectSctpJob;
    friend class RunTickJob;

    void protectAndSendRtp(uint64_t timestamp, memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator);
    void doProtectAndSend(uint64_t timestamp,
        memory::Packet* packet,
        const SocketAddress& target,
        Endpoint* endpoint,
        memory::PacketPoolAllocator& allocator);
    void sendPadding(uint64_t timestamp);
    void sendRtcpPadding(uint64_t timestamp, uint32_t ssrc, uint16_t nextPacketSize);

    void processRtcpReport(const rtp::RtcpHeader& packet,
        uint64_t timestamp,
        std::__1::chrono::system_clock::time_point wallClock);

    void doSetRemoteIce(const memory::Packet* credentials, const memory::Packet* const* candidatePackets);
    void doSetRemoteDtls(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide);

    bool doBandwidthEstimation(uint64_t receiveTime, const utils::Optional<uint32_t>& absSendTime, uint32_t packetSize);
    void doConnect();
    void doConnectSctp();
    void doRunTick(uint64_t timestamp);

    void appendRemb(memory::Packet* rtcpPacket,
        const uint64_t timestamp,
        uint32_t senderSsrc,
        const uint32_t* activeInbound,
        int activeInboundCount);

    void sendReports(uint64_t timestamp, bool rembReady = false);
    void sendRtcp(memory::Packet* rtcpPacket, memory::PacketPoolAllocator& allocator, const uint64_t timestamp);

    void onSendingRtcp(const memory::Packet& rtcpPacket, uint64_t timestamp);

    RtpReceiveState& getInboundSsrc(uint32_t ssrc);
    RtpSenderState& getOutboundSsrc(uint32_t ssrc, uint32_t rtpFrequency);

    void onTransportConnected();

    std::atomic_bool _isInitialized;
    logger::LoggableId _loggableId;
    size_t _endpointIdHash;
    const config::Config& _config;

    std::unique_ptr<SrtpClient> _srtpClient;
    bool _dtlsEnabled;
    std::unique_ptr<ice::IceSession> _rtpIceSession;

    typedef std::vector<Endpoint*> Endpoints;
    Endpoints _rtpEndpoints;
    Endpoints _rtcpEndpoints;
    std::vector<ServerEndpoint*> _tcpServerEndpoints;
    TcpEndpointFactory* _tcpEndpointFactory;
    std::atomic_int _callbackRefCount;
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
    uint32_t _outboundRembEstimateKbps;
    utils::RateTracker<10> _sendRateTracker; // B/ns
    uint64_t _lastLogTimestamp;
    std::atomic_uint32_t _rttNtp;

    concurrency::MpmcHashmap32<uint32_t, RtpSenderState> _outboundSsrcCounters;
    concurrency::MpmcHashmap32<uint32_t, RtpReceiveState> _inboundSsrcCounters;

    std::atomic_bool _isRunning;

    struct
    {
        uint8_t payloadType;
        uint32_t rtpFrequency;
    } _audio;
    utils::Optional<uint8_t> _absSendTimeExtensionId;

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

    struct PacketInfo
    {
        memory::Packet* packet;
        memory::PacketPoolAllocator& allocator;
    };
    memory::RandomAccessBacklog<PacketInfo, 512> _pacingQueue;
    memory::RandomAccessBacklog<PacketInfo, 512> _rtxPacingQueue;
    std::atomic_bool _pacingInUse;

    std::unique_ptr<logger::PacketLoggerThread> _packetLogger;
#ifdef DEBUG
public:
    concurrency::MutexGuard _singleThreadMutex;
#endif
};

} // namespace transport
